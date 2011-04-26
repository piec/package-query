/*
 *  aur.c
 *
 *  Copyright (c) 2010-2011 Tuxce <tuxce.net@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"
#include <string.h>
#include <alpm_list.h>
#include <pthread.h>
#include <errno.h>


#include <curl/curl.h>
#include <curl/types.h>
#include <curl/easy.h>
#include <yajl/yajl_parse.h>
#include <yajl/yajl_gen.h>

#include "aur.h"
#include "alpm-query.h"
#include "util.h"

/*
 * AUR url
 */
#define AUR_RPC	"/rpc.php"
#define AUR_RPC_SEARCH "?type=search&arg="
#define AUR_RPC_INFO "?type=info&arg="
#define AUR_URL_ID	"/packages.php?setlang=en&ID="

/*
 * AUR PAGE
 */
#define AUR_M_START "<span class='f3'>Maintainer: "
#define AUR_M_END   "</span>"
#define AUR_M_NONE  "None"

/*
 * AUR repo name
 */
#define AUR_REPO "aur"

/*
 * AUR package information
 */
#define AUR_ID          "ID"
#define AUR_NAME        "Name"
#define AUR_VER         "Version"
#define AUR_CAT         "CategoryID"
#define AUR_DESC        "Description"
#define AUR_LOC         "LocationID"
#define AUR_URL         "URL"
#define AUR_URLPATH     "URLPath"
#define AUR_LICENSE     "License"
#define AUR_VOTE        "NumVotes"
#define AUR_OUT         "OutOfDate"
#define AUR_LAST_ID     AUR_OUT



/*
 * AUR REQUEST
 */
#define AUR_INFO        1
#define AUR_SEARCH      2

/*
 * AUR concurrent connections
 */
#ifndef AUR_MAX_CONNECT
#define AUR_MAX_CONNECT 10
#endif


/*
 * JSON parse packages
 */
#define AUR_ID_LEN 	20
typedef struct _jsonpkg_t
{
	alpm_list_t *pkgs;
	aurpkg_t *pkg;
	char current_key[AUR_ID_LEN];
	int level;
} jsonpkg_t;


static alpm_list_t *reqs=NULL;
static target_arg_t *ta=NULL;
static int aur_pkgs_found_count=0;

static pthread_mutex_t aur_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t aur_mp = PTHREAD_MUTEX_INITIALIZER;

typedef struct _request_t
{
	int type;
	char *arg;
	const char *target;
	alpm_list_t *list_t;
	target_t *t_info;
	alpm_list_t *pkgs;
} request_t;

static request_t *request_new ()
{
	request_t *req=NULL;
	MALLOC (req, sizeof(request_t));
	req->type = 0;
	req->arg = NULL;
	req->target = NULL;
	req->list_t = NULL;
	req->t_info = NULL;
	req->pkgs = NULL;

	return req;
}

static void request_free (request_t *req)
{
	if (req == NULL)
		return;

	FREE (req->arg);
	target_free (req->t_info);
	FREE (req);
}

aurpkg_t *aur_pkg_new ()
{
	aurpkg_t *pkg = NULL;
	MALLOC (pkg, sizeof(aurpkg_t));
	pkg->id = 0;
	pkg->name = NULL;
	pkg->version = NULL;
	pkg->category = 0;
	pkg->desc = NULL;
	pkg->location = 0;
	pkg->url = NULL;
	pkg->urlpath = NULL;
	pkg->license = NULL;
	pkg->votes = 0;
	pkg->outofdate = 0;
	pkg->maintainer = NULL;
	return pkg;
}

void aur_pkg_free (aurpkg_t *pkg)
{
	if (pkg == NULL)
		return;
	FREE (pkg->name);
	FREE (pkg->version);
	FREE (pkg->desc);
	FREE (pkg->url);
	FREE (pkg->urlpath);
	FREE (pkg->license);
	FREE (pkg->maintainer);
	FREE (pkg);
}

aurpkg_t *aur_pkg_dup (const aurpkg_t *pkg)
{
	if (pkg == NULL)
		return NULL;
	aurpkg_t *pkg_ret = aur_pkg_new();
	pkg_ret->id = pkg->id;
	pkg_ret->name = STRDUP (pkg->name);
	pkg_ret->version = STRDUP (pkg->version);
	pkg_ret->category = pkg->category;
	pkg_ret->desc = STRDUP (pkg->desc);
	pkg_ret->location = pkg->location;
	pkg_ret->url = STRDUP (pkg->url);
	pkg_ret->urlpath = STRDUP (pkg->urlpath);
	pkg_ret->license = STRDUP (pkg->license);
	pkg_ret->votes = pkg->votes;
	pkg_ret->outofdate = pkg->outofdate;
	pkg_ret->maintainer = STRDUP (pkg->maintainer);
	return pkg_ret;
}

int aur_pkg_cmp (const aurpkg_t *pkg1, const aurpkg_t *pkg2)
{
	return strcmp (aur_pkg_get_name (pkg1), aur_pkg_get_name (pkg2));
}

int aur_pkg_votes_cmp (const aurpkg_t *pkg1, const aurpkg_t *pkg2)
{
	if (pkg1->votes > pkg2->votes) 
		return 1;
	else if (pkg1->votes < pkg2->votes)
		return -1;
	return 0;
}

unsigned int aur_pkg_get_id (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->id;
	return 0;
}

const char * aur_pkg_get_name (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->name;
	return NULL;
}

const char * aur_pkg_get_version (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->version;
	return NULL;
}

const char * aur_pkg_get_desc (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->desc;
	return NULL;
}

const char * aur_pkg_get_url (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->url;
	return NULL;
}

const char * aur_pkg_get_urlpath (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->urlpath;
	return NULL;
}

const char * aur_pkg_get_license (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->license;
	return NULL;
}

unsigned int aur_pkg_get_votes (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->votes;
	return 0;
}

unsigned short aur_pkg_get_outofdate (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->outofdate;
	return 0;
}

const char * aur_pkg_get_maintainer (const aurpkg_t * pkg)
{
	if (pkg!=NULL)
		return pkg->maintainer;
	return NULL;
}

static void aur_fetch_type ()
{
	char *c;
	config.aur_fetch = AUR_FETCH_SIMPLE;
	if ( config.aur_orphan ||
	     ((c=strstr (config.format_out, "%m")) &&
	     (c==config.format_out || *(c-1) != '%'))
	   )
		config.aur_fetch = AUR_FETCH_LONG;
}


static size_t curl_getdata_cb (void *data, size_t size, size_t nmemb, void *userdata)
{
	string_t *s = (string_t *) userdata;
	string_ncat (s, data, nmemb);
	return nmemb;
}

static int json_start_map (void *ctx) 
{
	jsonpkg_t *pkg_json = (jsonpkg_t *) ctx;
	if (++(pkg_json->level)>1)
		pkg_json->pkg = aur_pkg_new();
	return 1;
}

static int json_end_map (void *ctx)
{
	jsonpkg_t *pkg_json = (jsonpkg_t *) ctx;
	if (--(pkg_json->level)==1)
	{
		switch (config.sort)
		{
			case 'w':
				pkg_json->pkgs = alpm_list_add_sorted (pkg_json->pkgs, 
			        pkg_json->pkg, (alpm_list_fn_cmp) aur_pkg_votes_cmp);
				break;
			default:
				pkg_json->pkgs = alpm_list_add_sorted (pkg_json->pkgs, 
				    pkg_json->pkg, (alpm_list_fn_cmp) aur_pkg_cmp);
		}
		pkg_json->pkg = NULL;
	}
	return 1;
}

static int json_key (void * ctx, const unsigned char * stringVal,
                            size_t stringLen)
{
	jsonpkg_t *pkg_json = (jsonpkg_t *) ctx;
	stringLen = (stringLen>=AUR_ID_LEN) ? AUR_ID_LEN-1 : stringLen;
	strncpy (pkg_json->current_key, (const char *) stringVal, stringLen);
	pkg_json->current_key[stringLen] = '\0';
    return 1;
}

static int json_value (void * ctx, const unsigned char * stringVal,
                           size_t stringLen)
{
	jsonpkg_t *pkg_json = (jsonpkg_t *) ctx;
	// package info in level 2
	if (pkg_json->level<2) return 1;
	char *s = strndup ((const char *)stringVal, stringLen);
	int free_s = 1;
	if (strcmp (pkg_json->current_key, AUR_ID)==0)
	{
		pkg_json->pkg->id = atoi (s);
	}
	else if (strcmp (pkg_json->current_key, AUR_NAME)==0)
	{
		pkg_json->pkg->name = s;
		free_s = 0;
	}
	else if (strcmp (pkg_json->current_key, AUR_VER)==0)
	{
		pkg_json->pkg->version = s;
		free_s = 0;
	}
	else if (strcmp (pkg_json->current_key, AUR_CAT)==0)
	{
		pkg_json->pkg->category = atoi (s);
	}
	else if (strcmp (pkg_json->current_key, AUR_DESC)==0)
	{
		pkg_json->pkg->desc = s;
		free_s = 0;
	}
	else if (strcmp (pkg_json->current_key, AUR_LOC)==0)
	{
		pkg_json->pkg->location = atoi(s);
	}
	else if (strcmp (pkg_json->current_key, AUR_URL)==0)
	{
		pkg_json->pkg->url = s;
		free_s = 0;
	}
	else if (strcmp (pkg_json->current_key, AUR_URLPATH)==0)
	{
		pkg_json->pkg->urlpath = s;
		free_s = 0;
	}
	else if (strcmp (pkg_json->current_key, AUR_LICENSE)==0)
	{
		pkg_json->pkg->license = s;
		free_s = 0;
	}
	else if (strcmp (pkg_json->current_key, AUR_VOTE)==0)
	{
		pkg_json->pkg->votes = atoi(s);
	}
	else if (strcmp (pkg_json->current_key, AUR_OUT)==0)
	{
		pkg_json->pkg->outofdate = atoi(s);
	}
	if (free_s)
		free (s);
	
    return 1;
}

static yajl_callbacks callbacks = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    json_value,
    json_start_map,
    json_key,
    json_end_map,
    NULL,
    NULL,
};

static alpm_list_t *aur_json_parse (const char *s)
{
	jsonpkg_t pkg_json = { NULL, NULL, "", 0};
	yajl_handle hand;
	yajl_status stat;
	hand = yajl_alloc(&callbacks, NULL, (void *) &pkg_json);
	stat = yajl_parse(hand, (const unsigned char *) s, strlen (s));
	if (stat == yajl_status_ok)
		stat = yajl_complete_parse(hand);
	if (stat != yajl_status_ok)
	{
		unsigned char * str = yajl_get_error(hand, 1, 
		    (const unsigned char *) s, strlen (s));
		fprintf(stderr, (const char *) str);
		yajl_free_error(hand, str);
		alpm_list_free_inner (pkg_json.pkgs, (alpm_list_fn_free) aur_pkg_free);
		alpm_list_free (pkg_json.pkgs);
		pkg_json.pkgs = NULL;
	}	
	yajl_free(hand);
	return pkg_json.pkgs;
}

static void aur_fetch_page (CURL *curl, aurpkg_t *pkg)
{
	if (config.aur_fetch != AUR_FETCH_LONG) return;
	char url[PATH_MAX];
	CURLcode curl_code;
	string_t *res;
	char *c1, *c2;
	sprintf (url, "%s%s%d", config.aur_url, AUR_URL_ID, aur_pkg_get_id (pkg));
	res = string_new();

	curl_easy_setopt (curl, CURLOPT_WRITEDATA, res);
	curl_easy_setopt (curl, CURLOPT_URL, (const char *) url);
	if ((curl_code = curl_easy_perform (curl)) != CURLE_OK)
	{
		fprintf(stderr, "curl error: %s\n", curl_easy_strerror (curl_code));
		string_free (res);
		return;
	}
	c1 = strstr (string_cstr (res), AUR_M_START);
	if (c1)
	{
		c1+= strlen (AUR_M_START);
		c2 = strstr (c1, AUR_M_END);
		if (c2)
		{
			if (strncmp (c1, AUR_M_NONE, c2-c1)!=0)
				pkg->maintainer = strndup (c1, c2-c1);
		}
	}
	string_free (res);
}


static int aur_fetch (CURL *curl, request_t *req)
{
	char url[PATH_MAX];
	CURLcode curl_code;
	string_t *res;
	if (req->type == AUR_SEARCH)
		sprintf (url, "%s%s%s", config.aur_url, AUR_RPC, AUR_RPC_SEARCH);
	else
		sprintf (url, "%s%s%s", config.aur_url, AUR_RPC, AUR_RPC_INFO);
	res = string_new();

	char *encoded_arg = curl_easy_escape (curl, req->arg, 0);
	if (encoded_arg == NULL)
	{
		string_free (res);
		return 0;
	}
	strcat (url, encoded_arg);
	curl_free (encoded_arg);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, res);
	curl_easy_setopt (curl, CURLOPT_URL, (const char *) url);
	if ((curl_code = curl_easy_perform (curl)) != CURLE_OK)
	{
		fprintf(stderr, "curl error: %s\n", curl_easy_strerror (curl_code));
		string_free (res);
		return 0;
	}
	req->pkgs = aur_json_parse (string_cstr (res));
	string_free (res);
	return (req->pkgs != NULL);
}

static int aur_parse (CURL *curl, request_t *req)
{
	alpm_list_t *p;
	int found = 0;
	if (!req->pkgs)
	{
		if (config.aur_foreign)
		{
			aurpkg_t *pkg = aur_pkg_new ();
			pkg->name = strdup (req->target);
			print_package (req->target, pkg, aur_get_str);
			aur_pkg_free (pkg);
		}	
		return 1;
	}
	if (req->type == AUR_INFO)
	{
		for (p = req->pkgs; p; p = alpm_list_next(p))
		{
			aurpkg_t *pkg = alpm_list_getdata (p);
			if (target_check_version (req->t_info, aur_pkg_get_version (pkg)))
			{
				if (target_arg_add (ta, req->target, (void *) aur_pkg_get_name (pkg)))
				{
					aur_fetch_page (curl, pkg);
					print_package (req->target, alpm_list_getdata (p), aur_get_str);
				}
				found++;
			}
		}
	}
	else
	{
		int match=0;
		/* Filter results with others targets without making more queries */
		for (p = req->pkgs; p; p = alpm_list_next(p))
		{
			alpm_list_t *t;
			match=1;
			for (t = req->list_t; t && match; t = alpm_list_next (t))
			{
				if (strcasestr (aur_pkg_get_name (alpm_list_getdata (p)), alpm_list_getdata (t))==NULL &&
					strcasestr (aur_pkg_get_desc (alpm_list_getdata (p)), alpm_list_getdata (t))==NULL)
					match=0;
			}
			if (match)
			{
				found++;
				aur_fetch_page (curl, alpm_list_getdata (p));
				print_or_add_result (alpm_list_getdata (p), R_AUR_PKG);
			}
		}
	}
	alpm_list_free_inner (req->pkgs, (alpm_list_fn_free) aur_pkg_free);
	alpm_list_free (req->pkgs);
	req->pkgs=NULL;
	return found;
}

static void *thread_aur_fetch (void *arg)
{
	request_t *req;
	CURL *curl;
	curl = curl_easy_init ();
	if (!curl)
	{
		perror ("curl");
		pthread_exit(NULL);
		return 0;
	}
	curl_easy_setopt (curl, CURLOPT_ENCODING, "gzip");
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, curl_getdata_cb);
	if (config.insecure)
		curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, 0);
	while (1)
	{
		pthread_mutex_lock(&aur_mutex);
		req = alpm_list_getdata (reqs);
		reqs = alpm_list_next (reqs);
		pthread_mutex_unlock(&aur_mutex);
		if (!req) break;
		if (aur_fetch (curl, req) || config.aur_foreign)
		{
			pthread_mutex_lock(&aur_mp);
			aur_pkgs_found_count += aur_parse (curl, req);
			pthread_mutex_unlock(&aur_mp);
		}
		request_free (req);
	}
	curl_easy_cleanup(curl);
	pthread_exit(NULL);
	return 0;
}

static int aur_request (alpm_list_t **targets, int type)
{
	int i, n;
	request_t *req;
	pthread_t *thread;
	pthread_attr_t attr;
	alpm_list_t *t, *req_list;

	if (targets == NULL)
		return 0;

	aur_pkgs_found_count = 0;
	ta = target_arg_init ((ta_dup_fn) strdup,
	                      (alpm_list_fn_cmp) strcmp,
	                      (alpm_list_fn_free) free);
	aur_fetch_type ();
	curl_global_init(CURL_GLOBAL_SSL);

	for(t = *targets; t; t = alpm_list_next(t)) 
	{
		req = request_new ();
		req->target = alpm_list_getdata(t);
		req->type = type;
		if (type == AUR_SEARCH)
		{
			char *c;
			req->arg = strdup (req->target);
			while ((c = strchr (req->arg, '*')) != NULL)
				*c='%';
		}
		else
		{
			req->t_info = target_parse (req->target);
			if (req->t_info->db && strcmp (req->t_info->db, AUR_REPO)!=0)
			{
				request_free (req);
				continue;
			}
			req->arg = strdup (req->t_info->name);
		}
		reqs = alpm_list_add (reqs, req);
		if (type == AUR_SEARCH)
		{
			/* Only search for the first target */
			req->list_t = alpm_list_next (t);
			break;
		}
	}
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	n = alpm_list_count (reqs);
	n = (n>AUR_MAX_CONNECT) ? AUR_MAX_CONNECT : n;
	CALLOC (thread, n, sizeof (pthread_t));
	req_list = reqs;
	for(i=0; i<n; i++) 
	{
		if (pthread_create(&thread[i], &attr, thread_aur_fetch, NULL) != 0)
		{
			perror ("pthread_create: ");
			exit (2);
		}
	}
	pthread_attr_destroy(&attr);
	for(i=0; i<n; i++) 
	{
		if (pthread_join(thread[i], NULL) != 0)
		{
			perror ("pthread_join: ");
			exit (2);
		}
	}
	FREE (thread);
	alpm_list_free (req_list);

	curl_global_cleanup();

	*targets = target_arg_close (ta, *targets);
	ta = NULL;
	return aur_pkgs_found_count;
}

int aur_info (alpm_list_t **targets)
{
	return aur_request (targets, AUR_INFO);
}

int aur_search (alpm_list_t *targets)
{
	return aur_request (&targets, AUR_SEARCH);
}

const char *aur_get_str (void *p, unsigned char c)
{
	aurpkg_t *pkg = (aurpkg_t *) p;
	static char *info=NULL;
	static int free_info=0;
	if (free_info)
	{
		free (info);
		free_info = 0;
	}
	info = NULL;
	switch (c)
	{
		case 'd': info = (char *) aur_pkg_get_desc (pkg); break;
		case 'i': 
			info = itostr (aur_pkg_get_id (pkg)); 
			free_info = 1;
			break;
		case 'm': info = (char *) aur_pkg_get_maintainer (pkg); break;
		case 'n': info = (char *) aur_pkg_get_name (pkg); break;
		case 'o': 
			info = itostr (aur_pkg_get_outofdate (pkg)); 
			free_info = 1;
			break;
		case 's':
		case 'r': info = strdup (AUR_REPO); free_info=1; break;
		case 'u': 
			info = (char *) malloc (sizeof (char) * 
				(strlen (config.aur_url) + 
				strlen (aur_pkg_get_urlpath (pkg)) +
				2 /* '/' separate url and filename */
				));
			strcpy (info, config.aur_url);
			strcat (info, aur_pkg_get_urlpath (pkg));
			free_info = 1;
			break;
		case 'V':
		case 'v': info = (char *) aur_pkg_get_version (pkg); break;
		case 'w': 
			info = itostr (aur_pkg_get_votes (pkg)); 
			free_info = 1;
			break;
		default: return NULL; break;
	}
	return info;
}

void aur_cleanup ()
{
	aur_get_str (NULL, 0);
}

/* vim: set ts=4 sw=4 noet: */
