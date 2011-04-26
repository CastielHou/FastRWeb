/*
 *  CGI interface to Rserve
 *  Copyright (C) 2004,2008,2011 Simon Urbanek, All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License or
 *  (at your option) AT&T Proprietary license.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  $Id$
 */

/* -- CHANGE the following two variables to match your setup or define
   PROJECT_ROOT and RSERVE_SOCK -- */

/* project root directory */
#ifndef PROJECT_ROOT
#define PROJECT_ROOT "/var/FastRWeb"
#endif
const char *root = PROJECT_ROOT;

/* socket name - must match the name used when starting Rserve */
#ifndef RSERVE_SOCK
#define RSERVE_SOCK "/var/FastRWeb/socket" 
#endif
const char *sock = RSERVE_SOCK;

/* -- end of user configuirable part -- */

/* The layout (relative to the project root):
   web.R    - contains R scripts .../cgi/<foo> request is translated
              to web.R/<foo>.R (multiple extensions are supported and
              desired, e.g. ../cgi/plot.png translates to plot.png.R)
   web      - static web content that can be addressed using the "file"
              directive
   tmp      - temporary working directory, used for exchanging
              temporary files using the "tmpfile" directove

   Scripts in web.R are
    1) 'source'd
    2) global variables are preset:
       qs (query string), pars (parameter list), cmd, ct, hdr
    3) run() function is called and its return value used 

   The result value must be a character vector of either form:
    a) exactly one element - this is assumed to be text to be passed
       back with the content type text/html and encoding UTF-8
    b) two or more elements. The elements are interpreted as follows:
       [1] command - must be one of "html","file","tmpfile" or "raw"
       [2] payload - the contents(=body) to send back
       [3] content-type - HTML content type (defaults to "text/html")
       [4] header - custom headers (use with care! must contain valid
           HTML headers, e.g. cookies etc.)

   A Hello World example here would be:
   run <- function() c("html", "<b>Hello, World!</b>")

   Valid commands:
     "html" - the payload is send as the body of the response. The
              content type defaults to text/html unless overridden
              by the third parameter
     "file" - the payload is expected to be in a given file in the
              "web" directory of the project root
     "tmpfile" - the payload is expected to be in a given file in
               the "tmp" directory of the project root. Also the
               file is automatically deleted after being loaded.
     "raw" - the payload is sent as-is with no headers, i.e. the
             payload must be a valid HTTP response

*/

#define MAIN         // we are the main program, we need to define this
#define SOCK_ERRORS  // we will use verbose socket errors

#include "sisocks.h"
#include "Rconnection.h"
#include <sys/time.h>

char sfb[32768];

struct timeval startT, stopT;

/* log access to the log/cgi.log file - if possible */
static void wlog(const char *cmd, const char *info) {
  char wfn[512], idb[16];
  snprintf(wfn, 512, "%s/logs/cgi.log", root);
  gettimeofday(&stopT, 0);
  double t1 = (double) startT.tv_usec; t1 *= 0.000001; t1 += (double) startT.tv_sec;
  double t2 = (double) stopT.tv_usec; t2 *= 0.000001; t2 += (double) stopT.tv_sec;
  t2 -= t1;
  FILE *f = fopen(wfn, "a");
  if (!f) return;
  /* you can tag your logs with some cookie content - as an exmaple we use userID cookie */
  char *s = getenv("HTTP_COOKIE");
  if (s) s = strstr(s, "userID=");
  if (s) {
    int i = 0;
    while (i < 8 && ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z'))) { idb[i] = s[i]; i++; }
    idb[0] = 0;
  }
  if (!s) s="";
  fprintf(f,"%u\t%.2f\t%s\t%s\t%s\t%s\t%s\t%s\n",
	  (unsigned int) time(0),
	  t2,
	  getenv("REMOTE_ADDR"),
	  s,
	  getenv("REQUEST_URI"),
	  cmd,
	  info,
	  getenv("HTTP_USER_AGENT"));
  fclose(f);
}

int main(int argc, char **argv) {
    gettimeofday(&startT, 0);
    char *pi = getenv("PATH_INFO");
    while (pi && *pi=='/') pi++; /* skip leading slashes in PATH_INFO */
    if (!pi || !*pi) {
		printf("Content-type: text/html\n\n<b>Error: no function or path specified.</b>\n");
		return 0;
    }
    initsocks(); // this is needed for Win32 - it does nothing on unix

    // use unix sockets
    Rconnection *rc = new Rconnection(sock, -1);
    
	// try to connect
    int i=rc->connect();
    if (i) { // show error page on failure
        char msg[256];
		snprintf(msg, 256, "%s/web/connection_down.html", root);
		FILE *f = fopen(msg, "r");
		if (f) { /* if web/connection_down.html exists, send it */
			int n = 0;
			while ((n = fread(sfb, 1, sizeof(sfb), f)) > 0)
				fwrite(sfb, 1, n, stdout);
			fclose(f);
			delete rc;
			return 0;
		}
        sockerrorchecks(msg, 256, -1);
        printf("Content-type: text/html\n\n<b>Unable to connect</b> (result=%d, socket:%s).\n", i, msg);
		delete rc;
		return 0;
    }
	
    /* we need to forward QUERY_STRING, REQUEST_URI and HTTP_COOKIE to Rserve since it has
       no access to the CGI environemnt variables as it's in a separate progess */
    char *qs = getenv("QUERY_STRING");
    char *sqs = "";
    if (qs && *qs) { /* sanitize query string - escape \, ', " and replace \r or \n by ' ' */
		sqs = (char *) malloc(strlen(qs)*2+2);
		char *c = qs, *d = sqs;
		while (*c) {
			*d=*c;
			if (*c=='\\') (d++)[0]='\\';
			else if (*c=='\'') { d[0]='\\'; (d++)[0]='\''; }
			else if (*c=='\r' || *c=='\n') *c=' ';
			c++; d++;
		}
		*d=0;
    }

	char *rqs = getenv("REQUEST_URI");
	char *srqs = "";
	if (rqs && *rqs) { /* sanitize request string - escape \, ', " and replace \r or \n by ' ' */
		srqs = (char *) malloc(strlen(rqs)*2+2);
		char *c = rqs, *d = srqs;
		while (*c) {
			*d = *c;
			if (*c == '\\') (d++)[0] = '\\';
			else if (*c == '\'') { d[0] = '\\'; (d++)[0]='\''; }
			else if (*c == '\r' || *c == '\n') *c=' ';
			c++; d++;
		}
		*d = 0;
	}
	
    char *cook = getenv("HTTP_COOKIE");
    char *scook = "";
    if (cook && *cook) { /* sanitize by URI-encoding dangerous characters */
      scook = (char*) malloc(strlen(cook) * 3 + 3); /* very conservative estimate */
      char *c = cook, *d = scook;
      while (*c) {
	if (*c < ' ' || *c=='\\' || *c=='\"' || *c=='\'') {
	  snprintf(d,4,"%%%02x",(int)((unsigned char)*c));
	  d+=2;
	} else *d = *c;
	c++; d++;
      }
      *d = 0;
    }

    char *pii = strdup(pi); /* sanitize path: replace .. by _. */
    { char *c=pii; while (*c) { if (c[0]=='.' && c[1]=='.') *c='_'; c++;  } }
	
	/* create R code to evaluate */
    snprintf(sfb, sizeof(sfb),
			 "{setwd('%s/tmp');" \
			 "library(FastRWeb);" \
			 ".out<-''; cmd<-'html'; ct<-'text/html'; hdr<-'';" \
			 "qs<-'%s';" \
			 "requestURI<-'%s';" \
		         "raw.cookies<-'%s';" \
			 "pars<-list();" \
			 "lapply(strsplit(strsplit(qs,\"&\")[[1]],\"=\"),function(x) pars[[x[1]]]<<-x[2]);" \
			 "if(exists('init') && is.function(init)) init();" \
			 "as.character(try({source('%s/web.R/%s.R'); as.WebResult(do.call(run, pars)) },silent=TRUE))}\n",
		 root, sqs, srqs, scook, root, pii);
	/* Note: for efficientcy we don't parse cookies. Use getCookies() to populate cookies. */
	int res = 0;
	
	/* evaluate the constructed code */
    Rstrings *x = (Rstrings*) rc->eval(sfb, &res);
    
    if (x) { // if everything was fine, we have the result
		char *cmd = x->stringAt(0);
		char *pay = x->stringAt(1);
		char *ct  = x->stringAt(2);
		char *hdr = x->stringAt(3);
		if (!ct) ct="text/html; charset=utf-8";
		if (!pay) pay="";
		if (hdr && *hdr) { /* useful for cookies etc. */
			char *c = hdr;
			while (*c) c++;
			c--;
			if (c < hdr) c = hdr; /* for empty strings */
			if (*c!='\n' && *c!='\r') /* if the header doesn't end with \n or \r then add \r\n */
				printf("%s\n", hdr);
			else
				fwrite(hdr, 1, strlen(hdr), stdout);
		}
		if (cmd) {
			if (!strcmp(cmd, "file") || !strcmp(cmd, "tmpfile")) {
				wlog(cmd, pay);
				if (*pay) {
					char buf[256];
					if (!strcmp(cmd, "tmpfile"))
						snprintf(buf, 256, "%s/tmp/%s", root, pay);
					else
						snprintf(buf, 256, "%s/web/%s", root, pay);
					FILE *f = fopen(buf, "rb");
					if (f) {
						int n = 0;
						printf("Content-type: %s\n\n", ct);
						while (!feof(f) && (n=fread(sfb, 1, 4096, f))>0)
							fwrite(sfb, 1, n, stdout);
						fclose(f);
						if (!strcmp(cmd, "tmpfile")) unlink(buf);
					} else {
						printf("Content-type: text/html\n\nFile %s not found\n", buf);
					}
				} else {
					printf("Content-type: text/html\n\nFile not specified\n");
				}
			} else if (!strcmp(cmd, "raw")) {
				wlog(cmd, "");
				fwrite(pay, 1, strlen(pay), stdout);
			} else if (!strcmp(cmd, "header")) {
				wlog(cmd, hdr ? hdr : "<empty-header>");
				printf("\n"); /* add another \n to terminate the header */
			} else if (!strcmp(cmd, "html")) {
				wlog(cmd, "");
				printf("Content-type: %s\n\n%s\n", ct, pay);
			} else { /* fall-back for anything unexpected */
				wlog("none",cmd);
				printf("Content-type: %s\n\n%s\n%s", ct, cmd, pay);
			}
		} else {
			wlog("empty","");
			printf("Content-type: text/html\n\nFunction failed (no result)\n");
		}
		delete x;
    } else {
		char ef[16];
		snprintf(ef, 16, "%d", res);
		wlog("efail",ef);
		printf("Content-type: text/html\n\nEvaluation failed with error code %d\n", res);
    }
    
    // dispose of the connection object - this implicitly closes the connection
    delete rc;
    return 0;
}
