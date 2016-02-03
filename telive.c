/* telive v1.6 - tetra live monitor
 * (c) 2014-2016 Jacek Lipkowski <sq5bpf@lipkowski.org>
 * Licensed under GPLv3, please read the file LICENSE, which accompanies
 * the telive program sources
 *
 * Changelog:
 * v1.6 - various fixes, add a star to the status line to show if there is network coverage, show afc info --sq5bpf
 * v1.5 - added 'z' key to clear learned info, added play lock file --sq5bpf
 * v1.4 - added frequency window --sq5bpf
 * v1.3 - add frequency info --sq5bpf
 * v1.2 - fixed various crashes, made timeouts configurable via env variables --sq5bpf
 * v1.1 - made some buffers bigger, ignore location with INVALID_POSITION --sq5bpf
 * v1.0 - add KML export --sq5bpf
 * v0.9 - add TETRA_KEYS, add option to filter playback, fixed crash on problems playing, report when the network parameters change too quickly --sq5bpf
 * v0.8 - code cleanups, add the verbose option, display for text sds in the status window  --sq5bpf
 * v0.7 - initial public release --sq5bpf
 *
 *
 * TODO:
 * ack! functions with no bounds checking, usage of popen, environment
 * variables instead of getopt(), spaghetti code - basically this should
 * be rewritten from scratch some day :)
 */


#include <fnmatch.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ncurses.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <sys/file.h>
#include <fcntl.h>

#include "telive.h"
#include "telive-zmq.h"

#define TELIVE_VERSION "1.6"

#define BUFLEN 8192
#define PORT 7379

/******* definitions *******/
int rec_timeout=30; /* after how long we stop to record a usage identifier */
int ssi_timeout=60; /* after how long we forget SSIs */
int idx_timeout=8; /* after how long we disable the active flag */
int curplaying_timeout=5; /* after how long we stop playing the current usage identifier */
int freq_timeout=600; /* how long we remember frequency info */
int receiver_timeout=60; /* how long we remember receiver info */


char *outdir;
char def_outdir[BUFLEN]="/tetra/in";
char *logfile;
char def_logfile[BUFLEN]="telive.log";
char *ssifile;
char def_ssifile[BUFLEN]="ssi_descriptions";
char ssi_filter[BUFLEN];
int use_filter=0;

char *kml_file=NULL;
char *kml_tmp_file=NULL;
int kml_interval;
int last_kml_save=0;
int kml_changed=0;
int freq_changed=0;

char *lock_file=NULL;
int lockfd=0;
int locked=0;

/* various times for scheduling in tickf() */
time_t last_1s_event=0;
time_t last_10s_event=0;
time_t last_1min_event=0;

#define max(a,b) \
	({ __typeof__ (a) _a = (a); \
	 __typeof__ (b) _b = (b); \
	 _a > _b ? _a : _b; })


int verbose=0;
WINDOW *msgwin=0; /* message window */
WINDOW *statuswin=0; /* status window */
WINDOW *titlewin=0; /* title window */
WINDOW *mainwin=0; /* main window */
WINDOW *freqwin=0; /* frequency window */
WINDOW *ssiwin=0; /* SSI window */
WINDOW *locwin=0; /* Location window */
WINDOW *displayedwin=0; /* the window currently displayed */
int ref; /* window refresh flag, set to refresh all windows */

char prevtmsg[BUFLEN]; /* contents of previous message */

struct {
	uint16_t mcc;
	uint16_t mnc;
	uint8_t colour_code;
	uint32_t dl_freq;
	uint32_t ul_freq;
	uint16_t la;
	time_t last_change;
	uint32_t changes;
} netinfo;

/* this structure is used to describe all known frequencies */
struct freqinfo {
	uint32_t dl_freq;
	uint32_t ul_freq;
	uint16_t mcc;
	uint16_t mnc;
	uint16_t la;
	time_t last_change;
	int reason;
	int rx;
	void *next;
	void *prev;
};

struct usi {
	unsigned int ssi[3];
	time_t ssi_time[3];
	time_t ssi_time_rec;
	int encr;
	int timeout;
	int active;
	int play;
	char curfile[BUFLEN];
	char curfiletime[32];
};

struct opisy {
	unsigned int ssi;
	char *opis;
	void *next;
	void *prev;
};

struct locations {
	unsigned int ssi;
	float lattitude;
	float longtitude;
	char *description;
	time_t lastseen;
	void *next;
	void *prev;

};

/* reeciver info */
struct receiver {
	unsigned int rxid;
	int afc;
	uint32_t freq;
	time_t lastseen;
	void *next;
	void *prev;
};



struct receiver *receivers=NULL;
struct opisy *opisssi;
struct locations *kml_locations=NULL;
struct freqinfo *frequencies=NULL;

int curplayingidx=0;
time_t curplayingtime=0;
int curplayingticks=0;
FILE *playingfp=NULL;
int mutessi=0;
int alldump=0;
int ps_record=0;
int ps_mute=0;
int do_log=0;
int last_burst=0;

enum telive_screen { DISPLAY_IDX, DISPLAY_FREQ, DISPLAY_END };
int display_state=DISPLAY_IDX;

void appendlog(char *msg) {
	FILE *f;
	f=fopen(logfile,"ab");
	if (f) {
		fprintf(f,"%s\n",msg);
		fclose(f);
	}
}


void clearopisy()
{
	struct opisy *ptr,*ptr2;
	ptr=opisssi;

	while(ptr)
	{
		ptr2=ptr->next;
		free((void *)ptr->opis);
		free(ptr);
		ptr=ptr2;
	}
	opisssi=0;
}

time_t ostopis=0;

int newopis()
{
	struct stat st;
	if (stat(ssifile,&st)) return(0);

	if (st.st_mtime>ostopis) {
		ostopis=st.st_mtime;
		return(1);
	}
	return(0);
}

int initopis()
{
	FILE *g;
	char str[100];
	struct opisy *ptr,*prevptr;
	char *c;

	prevptr=NULL;
	clearopisy();
	g=fopen(ssifile,"r");
	if (!g) return(0);
	str[sizeof(str)-1]=0;
	while(!feof(g))
	{
		if (!fgets(str,sizeof(str)-1,g)) break;
		c=strchr(str,' ');
		if (c==NULL) continue;
		*c=0;
		c++;
		ptr=malloc(sizeof(struct opisy));
		if (prevptr) {
			prevptr->next=ptr;
			ptr->prev=prevptr;
			ptr->next=NULL;
		} else
		{
			opisssi=ptr;
		}
		ptr->ssi=atoi(str);
		ptr->opis=strdup(c);
		prevptr=ptr;
	}
	fclose(g);
	return(1);
}

const char nop[2]="-\0";
char *lookupssi(int ssi)
{
	struct opisy *ptr=opisssi;

	while(ptr)
	{
		if (ptr->ssi==ssi) return(ptr->opis);
		ptr=ptr->next;
	}
	return((char *)&nop);
}


void add_location(int ssi,float lattitude,float longtitude,char *description)
{
	struct locations *ptr=kml_locations;
	struct locations *prevptr=NULL;
	char *c;

	if (ptr) {
		/* maybe we already know this ssi? */
		while(ptr) {
			if (ptr->ssi==ssi) break;
			prevptr=ptr;
			ptr=ptr->next;
		}
	}

	if (!ptr) {
		ptr=calloc(1,sizeof(struct locations));
		if (!kml_locations) kml_locations=ptr;
		if (prevptr) {
			ptr->prev=prevptr;
			prevptr->next=ptr;
		}
		ptr->ssi=ssi;
	} else {
		free(ptr->description);
	}

	ptr->lastseen=time(0);
	ptr->lattitude=lattitude;
	ptr->longtitude=longtitude;
	ptr->description=strdup(description);
	c=ptr->description;
	/* ugly hack so that we don't get <> there, which would break the xml */
	while(*c) { if (*c=='>') *c='G';  if (*c=='<') *c='L'; c++; }
	kml_changed=1;

}

void dump_kml_file() {
	FILE *f;
	struct locations *ptr=kml_locations;
	if (verbose>1) wprintw(statuswin,"called dump_kml_file()\n");

	if (!kml_tmp_file) return;
	f=fopen(kml_tmp_file,"w");
	if (!f) return;
	if (verbose>1) wprintw(statuswin,"dump_kml_file(%s)\n",kml_tmp_file);
	fprintf(f,"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Folder>\n<name>");
	fprintf(f,"Telive MCC:%5i MNC:%5i ColourCode:%3i Down:%3.4fMHz Up:%3.4fMHz LA:%5i",netinfo.mcc,netinfo.mnc,netinfo.colour_code,netinfo.dl_freq/1000000.0,netinfo.ul_freq/1000000.0,netinfo.la);
	fprintf(f,"</name>\n<open>1</open>\n");

	while(ptr)
	{
		fprintf(f,"<Placemark> <name>%i</name> <description>%s %s</description> <Point> <coordinates>%f,%f,0</coordinates></Point></Placemark>\n",ptr->ssi,lookupssi(ptr->ssi),ptr->description,ptr->longtitude,ptr->lattitude);
		ptr=ptr->next;
	}

	fprintf(f,"</Folder></kml>\n");
	fclose(f);
	rename(kml_tmp_file,kml_file);
	last_kml_save=time(0);
	kml_changed=0;
}

/* delete the whole location info */
void clear_locations() {
	struct locations *ptr=kml_locations;
	struct locations *nextptr;
	while(ptr) {
		nextptr=ptr->next;
		free(ptr->description);
		free(ptr);
		ptr=nextptr;
	}
	kml_locations=NULL;
}

#define MAXUS 64
struct usi ssis[MAXUS];

void diep(char *s)
{
	wprintw(statuswin,"DIE: %s\n",s);
	wrefresh(statuswin);
	sleep(1);
	perror(s);
	exit(1);
}
#define RR 13
int getr(int idx) {
	return((idx%RR)*4);
}

int getcl(int idx)
{
	return ((idx/RR)*40);
}

/* update usage identifier display */
void updidx(int idx) {
	char opis[40];
	int i;
	int row=getr(idx);
	int col=getcl(idx);
	int bold=0;

	if ((idx<0)||(idx>=MAXUS)) { wprintw(statuswin,"BUG! updidx(%i)\n",idx); wrefresh(statuswin); return; }

	opis[0]=0;
	wmove(mainwin,row,col+5);
	if (ssis[idx].active) strcat(opis,"OK ");
	//	if (ssis[idx].timeout) strcat(opis,"timeout ");
	if (ssis[idx].encr) strcat(opis,"ENCR ");
	if ((ssis[idx].play)&&(ssis[idx].active)) { bold=1; strcat(opis,"*PLAY*"); }
	if (bold) wattron(mainwin,A_BOLD|COLOR_PAIR(1));
	wprintw(mainwin,"%-30s",opis);
	if (bold) wattroff(mainwin,COLOR_PAIR(1));
	for (i=0;i<3;i++) {
		wmove(mainwin,row+i+1,col+5);
		if (ssis[idx].ssi[i]) {
			wprintw(mainwin,"%8i %20s",ssis[idx].ssi[i],lookupssi(ssis[idx].ssi[i]));
		}
		else
		{
			wprintw(mainwin,"                              ");

		}
	}
	if (bold) wattroff(mainwin,A_BOLD);
	ref=1;
}

void draw_idx() {
	int idx;
	for (idx=0;idx<MAXUS;idx++)
	{
		wmove(mainwin,getr(idx),getcl(idx));
		wprintw(mainwin,"%2i:",idx);
	}
}

/* refresh the main window */
void display_mainwin() {
	int idx;
	wclear(mainwin);
	draw_idx();
	for (idx=0;idx<MAXUS;idx++) updidx(idx);
	ref=1;
}

#define STATUSLINES 7
int initcur() {
	int maxx,maxy;
	initscr();
	start_color();
	cbreak();
	init_pair(1, COLOR_RED, COLOR_BLUE);
	init_pair(2, COLOR_YELLOW, COLOR_BLUE);
	init_pair(3, COLOR_RED, COLOR_GREEN);
	init_pair(4, COLOR_WHITE, COLOR_GREEN);
	init_pair(5, COLOR_BLACK, COLOR_YELLOW);
	clear();
	titlewin=newwin(1,COLS,0,0);
	mainwin=newwin(LINES-STATUSLINES-1,COLS,1,0);
	freqwin=newwin(LINES-STATUSLINES-1,COLS,1,0);
	locwin=newwin(LINES-STATUSLINES-1,COLS,1,0);
	ssiwin=newwin(LINES-STATUSLINES-1,COLS,1,0);
	displayedwin=mainwin;
	msgwin=newwin(STATUSLINES,COLS/2,LINES-STATUSLINES,0);
	wattron(msgwin,COLOR_PAIR(2));
	wbkgdset(msgwin,COLOR_PAIR(2));
	wclear(msgwin);
	wprintw(msgwin,"Message window\n");
	scrollok(msgwin,TRUE);

	statuswin=newwin(STATUSLINES,COLS/2,LINES-STATUSLINES,COLS/2+1);
	wattron(statuswin,COLOR_PAIR(3));
	wbkgdset(statuswin,COLOR_PAIR(3));
	wclear(statuswin);
	wprintw(statuswin,"####  Press ? for keystroke help  ####\n");
	getmaxyx(stdscr,maxy,maxx);
	if ((maxx!=203)||(maxy!=60)) {
		wprintw(statuswin,"\nWARNING: The terminal size is %ix%i, but it should be 203x60\nThe program will still work, but the display may be mangled\n\n",maxx,maxy);
	}
	scrollok(statuswin,TRUE);

	wattron(titlewin,A_BOLD);
	wprintw(titlewin,"** SQ5BPF TETRA Monitor %s **",TELIVE_VERSION);
	wattroff(titlewin,A_BOLD);

	draw_idx();
	ref=1;
	return(1);
}

void updopis()
{
	wmove(titlewin,0,32);
	wattron(titlewin,COLOR_PAIR(4)|A_BOLD);
	wprintw(titlewin,"MCC:%5i MNC:%5i ColourCode:%3i Down:%3.4fMHz Up:%3.4fMHz LA:%5i %c",netinfo.mcc,netinfo.mnc,netinfo.colour_code,netinfo.dl_freq/1000000.0,netinfo.ul_freq/1000000.0,netinfo.la,last_burst?'*':' ');
	wattroff(titlewin,COLOR_PAIR(4)|A_BOLD);
	wprintw(titlewin," mutessi:%i alldump:%i mute:%i record:%i log:%i verbose:%i lock:%i",mutessi,alldump,ps_mute,ps_record,do_log,verbose,locked);
	switch(use_filter)
	{
		case 0:	wprintw(titlewin," no filter"); break;
		case  1:	wprintw(titlewin," Filter ON"); break;
		case -1:	wprintw(titlewin," Inv. Filt"); break;
	}
	wprintw(titlewin," [%s] ",ssi_filter);

	ref=1;
}

int addssi(int idx,int ssi)
{
	int i;

	if (!ssi) return(0);
	if ((idx<0)||(idx>=MAXUS)) { wprintw(statuswin,"BUG! addssi(%i,%i)\n",idx,ssi); wrefresh(statuswin); return(0); }
	for(i=0;i<3;i++) {
		if (ssis[idx].ssi[i]==ssi) {
			ssis[idx].ssi_time[i]=time(0);
			return(1);
		}
	}

    telive_zmq_msg ("ADD", ssi);

	for(i=0;i<3;i++) {
		if (!ssis[idx].ssi[i]) {
			ssis[idx].ssi[i]=ssi;
			ssis[idx].ssi_time[i]=time(0);
			return(1);
		}
	}



	/* no room to add, forget one ssi */
	ssis[idx].ssi[0]=ssis[idx].ssi[1];
	ssis[idx].ssi[1]=ssis[idx].ssi[2];
	ssis[idx].ssi[2]=ssi;
	ssis[idx].ssi_time[2]=time(0);
	ssis[idx].active=1;
	return(1);
}

int addssi2(int idx,int ssi,int i)
{
	if (!ssi) return(0);
	ssis[idx].ssi[i]=ssi;
	ssis[idx].ssi_time[i]=time(0);
	ssis[idx].active=1;
	return(0);
}


int releasessi(int ssi)
{
	int i,j;
	for (i=0;i<MAXUS;i++) {
		for (j=0;j<3;j++) {
			if ((ssis[i].active)&&(ssis[i].ssi[j]==ssi)) {
				ssis[i].active=0;
				ssis[i].play=0;
				updidx(i);
				ref=1;

                telive_zmq_msg ("RELEASE", ssi);
			}
		}
	}
	return(0);
}

/* check if an SSI matches the filter expression */
int matchssi(int ssi)
{
	int r;
	char ssistr[16];
	if (!ssi) return(0);
	sprintf(ssistr,"%i",ssi);
	if (strlen(ssi_filter)==0) return(1);

#ifdef FNM_EXTMATCH
	r=fnmatch((char *)&ssi_filter,(char *)&ssistr,FNM_EXTMATCH);
#else
	/* FNM_EXTMATCH is a GNU libc extension, not present in other libcs, like the MacOS X one */
#warning -----------    Extended match patterns for fnmatch are not supported by your libc. You will have to live with that.    ------------
	r=fnmatch((char *)&ssi_filter,(char *)&ssistr,0);
#endif
	return(!r);
}

/* check if any SSIs for this usage identifier match the filter expression */
int matchidx(int idx)
{
	int i;
	int j=0;
	if (!use_filter) return (1);
	for(i=0;i<3;i++) {
		if (matchssi(ssis[idx].ssi[i])) {
			j=1;
			break;
		}
	}
	if (use_filter==-1) j=!j;
	return(j);
}

/* locking functions */
int trylock() {
	int i;
	if (!lockfd) return(1);
	i=flock(lockfd,LOCK_EX|LOCK_NB);
	if (!i) { locked=1; updopis(); return(1); }
	return(0);
}

void releaselock() {
	int i=locked;
	locked=0;
	if (i) updopis();
	if (!lockfd) return;
	flock(lockfd,LOCK_UN);
}

/* receiver table functions */
void update_receivers(int rx,int afc,uint32_t freq)
{
	struct receiver *ptr=receivers;
	struct receiver *prevptr;

	/* do we know this rx? */
	while(ptr)
	{
		if (ptr->rxid==rx) break;
		prevptr=ptr;
		ptr=ptr->next;
	}
	/* nope, new one */
	if (!ptr) {
		ptr=calloc(1,sizeof(struct receiver));
		if (!receivers) {

			receivers=ptr;
		} else {
			prevptr->next=ptr;
			ptr->prev=prevptr;
		}
		ptr->rxid=rx;
	}
	ptr->afc=afc;
	if (freq)	ptr->freq=freq;
	ptr->lastseen=time(0);
}

/* time out old receivers */
void timeout_receivers() {
	struct receiver *ptr=receivers;
	struct receiver *prevptr,*nextptr;
	if (!ptr) return;

	time_t timenow=time(0);

	while(ptr) {
		if ((timenow-ptr->lastseen)>receiver_timeout)
		{
			prevptr=ptr->prev;
			nextptr=ptr->next;
			free(ptr);
			if (prevptr) {
				prevptr->next=nextptr;
				if (nextptr) nextptr->prev=prevptr;
			} else {
				receivers=nextptr;
				if (nextptr) nextptr->prev=NULL;
			}
		}
		ptr=ptr->next;
	}
}

/* clear all known receivers */
void clear_all_receivers() {
	struct receiver *ptr=receivers;
	struct receiver *ptr2;

	while(ptr) {
		ptr2=ptr;
		ptr=ptr->next;
		free(ptr2);

	}
	receivers=0;
}


/* frequency table functions */
#define REASON_NETINFO 1<<0
#define REASON_FREQINFO 1<<1
#define REASON_DLFREQ 1<<2
/* insert frequency into the freq table */
insert_freq(int reason,uint16_t mnc,uint16_t mcc,uint32_t ulf,uint32_t dlf,uint16_t la, int rx)
{

	struct freqinfo *ptr=frequencies;
	struct freqinfo *prevptr=NULL;
	char *c;

	/* maybe we already know the uplink or downlink frequency */
	while(ptr) {
		if ((ulf)&&(ulf==ptr->ul_freq)) break;
		if ((dlf)&&(dlf==ptr->dl_freq)) break;
		prevptr=ptr;
		ptr=ptr->next;
	}

	if (!ptr) {
		ptr=calloc(1,sizeof(struct freqinfo));
		if (!frequencies) frequencies=ptr;
		if (prevptr) {
			ptr->prev=prevptr;
			prevptr->next=ptr;
		}
	} else {

	}
	if ((verbose)&&(reason!=REASON_NETINFO)) {
		wprintw(statuswin,"Down:%3.4fMHz Up:%3.4fMHz LA:%i MCC:%i MNC:%i reason:%i RX:%i\n",dlf/1000000.0,ulf/1000000.0,la,mcc,mnc,reason,rx);
		wrefresh(statuswin);
	}
	ptr->last_change=time(0);
	ptr->ul_freq=ulf;
	ptr->dl_freq=dlf;
	ptr->la=la;
	ptr->mcc=mcc;
	ptr->mnc=mnc;
	ptr->reason=ptr->reason|reason;
	ptr->rx=rx;
	freq_changed=1;
}

/* clear the freq table, delete old frequencies */
void clear_freqtable() {
	struct freqinfo *ptr=frequencies;
	struct freqinfo *prevptr,*nextptr;
	int del;
	if (!ptr) return;

	time_t timenow=time(0);

	while(ptr) {
		del=0;
		if ((timenow-ptr->last_change)>freq_timeout) del=1; /* timed out */
		if ((!ptr->ul_freq)&&(!ptr->dl_freq)) del=1; /* no frequency info */
		if (del) {
			prevptr=ptr->prev;
			nextptr=ptr->next;
			free(ptr);
			if (prevptr) {
				prevptr->next=nextptr;
				if (nextptr) nextptr->prev=prevptr;
			} else {
				frequencies=nextptr;
				if (nextptr) nextptr->prev=NULL;
			}
			freq_changed=1;
		}
		ptr=ptr->next;
	}
}

/* delete the whole frequency table */
void clear_all_freqtable() {
	struct freqinfo *ptr=frequencies;
	struct freqinfo *nextptr;

	while(ptr) {
		nextptr=ptr->next;
		free(ptr);
		ptr=nextptr;
	}
	frequencies=NULL;
}

void display_freq() {
	char tmpstr2[64];
	char tmpstr[256];
	struct freqinfo *ptr=frequencies;
	struct receiver *rptr=receivers;
	wclear(freqwin);
	wprintw(freqwin,"\n***  Known frequencies:  ***\n");
	while(ptr) {
		tmpstr[0]=0;
		if (ptr->dl_freq) {
			sprintf(tmpstr2,"Downlink:%3.4fMHz\t",ptr->dl_freq/1000000.0);
			strcat(tmpstr,tmpstr2);
		}  else {
			strcat(tmpstr,"                    \t");
		}

		if (ptr->ul_freq) {
			sprintf(tmpstr2,"Uplink:%3.4fMHz\t",ptr->ul_freq/1000000.0);
			strcat(tmpstr,tmpstr2);
		} else {
			strcat(tmpstr,"                  \t");
		}

		if (ptr->mcc) {
			sprintf(tmpstr2,"MCC:%5i\t",ptr->mcc);
			strcat(tmpstr,tmpstr2);
		} else {
			strcat(tmpstr,"         \t");
		}


		if (ptr->mnc) {
			sprintf(tmpstr2,"MNC:%5i\t",ptr->mnc);
			strcat(tmpstr,tmpstr2);
		} else {
			strcat(tmpstr,"         \t");
		}


		if (ptr->la) {
			sprintf(tmpstr2,"LA:%5i\t",ptr->la);
			strcat(tmpstr,tmpstr2);
		} else {
			strcat(tmpstr,"        \t");
		}

		strcat(tmpstr,"reason:[");
		if (ptr->reason&REASON_NETINFO) strcat(tmpstr,"S");
		if (ptr->reason&REASON_FREQINFO) strcat(tmpstr,"N");
		if (ptr->reason&REASON_DLFREQ) strcat(tmpstr,"A");
		strcat(tmpstr,"]\t");
		sprintf(tmpstr2,"RX:%i",ptr->rx);
		strcat(tmpstr,tmpstr2);
		wprintw(freqwin,"%s\n",tmpstr);
		ptr=ptr->next;
	}

	wprintw(freqwin,"\n\n***    Receiver info:    ***\n\n");
	wprintw(freqwin,"RX:\tAFC:\t\t\t\tFREQUENCY\n");

	while(rptr) {

		char buf[24]="[..........:..........]";
		int i;
		i=(11+rptr->afc/10);
		if (i<2) { buf[2]='<'; }
		else  if (i>21) { buf[21]='>'; } else { buf[i]='|';  }

		sprintf(tmpstr,"%i\t%+3.3i %s",rptr->rxid,rptr->afc,buf);
		if (rptr->freq) {
			sprintf(tmpstr2,"%3.4fMHz",rptr->freq/1000000.0);
			strcat(tmpstr,tmpstr2);
		}
		wprintw(freqwin,"%s\n",tmpstr);
		rptr=rptr->next;
	}


	wattron(freqwin,A_BOLD);
	wmove(freqwin,1,170);
	wprintw(freqwin,"reasons: N:D-NWRK-BROADCAST");
	wmove(freqwin,2,170);
	wprintw(freqwin,"S:SYSINFO A:ChanAlloc");
	wattroff(freqwin,A_BOLD);

	ref=1;
	freq_changed=0;
}

/* find an usage identifier with live audio so that we can play it */
int findtoplay(int first)
{
	int i;
	if (curplayingidx) return(0);
	releaselock();

	for (i=first;i<MAXUS;i++) {
		if ((ssis[i].active)&&(!ssis[i].encr)&&(matchidx(i)&&(trylock()))) {
			curplayingidx=i;
			if (verbose>0) wprintw(statuswin,"NOW PLAYING %i\n",i);
			ref=1;
			return(1);
		}
	}
	for (i=0;i<first;i++) {
		if ((ssis[i].active)&&(!ssis[i].encr)&&(matchidx(i)&&(trylock()))) {
			curplayingidx=i;
			if (verbose>0) wprintw(statuswin,"NOW PLAYING %i\n",i);
			ref=1;
			return(1);
		}
	}
	curplayingidx=0;
	return(0);
}

void timeout_ssis(time_t t)
{
	int i,j;
	for (i=0;i<MAXUS;i++) {
		for (j=0;j<3;j++) {
			if ((ssis[i].ssi[j])&&(ssis[i].ssi_time[j]+ssi_timeout<t)) {
                telive_zmq_msg("TIMEOUT", ssis[i].ssi[j]);

				ssis[i].ssi[j]=0;
				ssis[i].ssi_time[j]=0;
				updidx(i);
				ref=1;
			}

		}
		/* move the array elements so that the indexes start at 0 */
		for (j=0;j<2;j++) {
			if (!ssis[i].ssi[j]) {
				ssis[i].ssi[j]=ssis[i].ssi[j+1];
				ssis[i].ssi_time[j]=ssis[i].ssi_time[j+1];
				ssis[i].ssi[j+1]=0;
				ssis[i].ssi_time[j+1]=0;
			}
		}
	}
}

void timeout_idx(time_t t)
{
	int i;
	for (i=0;i<MAXUS;i++) {
		if ((ssis[i].active)&&(ssis[i].timeout+idx_timeout<t)) {
			ssis[i].active=0;
			ssis[i].play=0;
			updidx(i);
			ref=1;
		}
	}

}

void timeout_curplaying(time_t t)
{
	if ((curplayingidx)&&(curplayingtime+curplaying_timeout<t)) {
		//wprintw(statuswin,"STOP PLAYING %i\n",curplayingidx);
		ssis[curplayingidx].active=0;
		ssis[curplayingidx].play=0;
		updidx(curplayingidx);
		ref=1;
		/* find another to play */
		curplayingidx=0;
		findtoplay(curplayingidx);
	}
}

/* timing out the recording */
void timeout_rec(time_t t)
{
	char tmpfile[256];
	int i;
	for (i=0;i<MAXUS;i++) {
		if ((strlen(ssis[i].curfile))&&(ssis[i].ssi_time_rec+rec_timeout<t)) {
			snprintf(tmpfile,sizeof(tmpfile),"%s/traffic_%s_%i_%i_%i_%i.out",outdir,ssis[i].curfiletime,i,ssis[i].ssi[0],ssis[i].ssi[1],ssis[i].ssi[2]);
			rename(ssis[i].curfile,tmpfile);
			ssis[i].curfile[0]=0;
			ssis[i].active=0;
			updidx(i);
			if(verbose>1) wprintw(statuswin,"timeout rec %s\n",tmpfile);
			ref=1;
		}
	}
}

void refresh_scr()
{
	ref=0;
	wrefresh(titlewin);
	wrefresh(displayedwin);
	wrefresh(statuswin);
	wrefresh(msgwin);
}

/* reopen a pipe to tplay */
void do_popen() {
	if (playingfp) pclose(playingfp);
	playingfp=popen("tplay >/dev/null 2>&1","w");
	if (!playingfp) {
		wprintw(statuswin,"PLAYBACK PROBLEM!! (fix tplay)\n");
		playingfp=NULL;
		curplayingidx=0;
	}
}


void tickf ()
{
	time_t t=time(0);
	if (curplayingidx) {
		/* crude hack to hack around buffering, i know this should be done better */
		if (curplayingticks==2) do_popen();

		curplayingticks++;
	}

	if ((t-last_1s_event)>0) {
		/* this gets executed every second */
		clear_freqtable();
		timeout_ssis(t);
		timeout_idx(t);
		last_1s_event=t;
		if (displayedwin==freqwin) display_freq();
	}
	if ((t-last_10s_event)>9) {
		/* this gets executed every 10 seconds */
		timeout_receivers();
		//if ((freq_changed)&&(displayedwin==freqwin)) display_freq();
		last_10s_event=t;
	}
	if ((t-last_1min_event)>59) {
		/* this gets executed every minute */
		ref=1;
		last_1min_event=t;
	}

	timeout_curplaying(t);
	timeout_rec(t);
	if (ref) refresh_scr();
	if (last_burst) {
		if (last_burst==1) {
			last_burst--;
			wprintw(statuswin,"Signal lost\n");
			updopis();
		} else {
			last_burst--;
		}
	}
	if ((kml_changed)&&(kml_interval)&&((t-last_kml_save)>kml_interval)) dump_kml_file();


}

void keyf(unsigned char r)
{
	time_t tp;
	char tmpstr[40];
	char tmpstr2[80];
	switch (r) {
		case 'l':
			do_log=!do_log;
			updopis();

			tp=time(0);
			strftime(tmpstr,sizeof(tmpstr),"%Y%m%d %H:%M:%S",localtime(&tp));
			if (do_log)
			{
				sprintf(tmpstr2,"%s **** log start ****",tmpstr);
			} else {

				sprintf(tmpstr2,"%s **** log end ****",tmpstr);
			}
			appendlog(tmpstr2);
			break;
		case 'M':
			ps_mute=!ps_mute;
			updopis();
			break;
		case 'R':
			ps_record=!ps_record;
			updopis();
			break;
		case 'm':
			mutessi=!mutessi;
			updopis();
			break;
		case 'a':
			alldump=!alldump;
			updopis();
			break;
		case 'v':
			if (verbose) verbose--;
			updopis();
			break;
		case 'V':
			if (verbose<4) verbose++;
			updopis();
			break;
		case 'r': /* refresh screen */
			redrawwin(statuswin);
			redrawwin(msgwin);
			redrawwin(mainwin);
			break;
		case 's': /* stop current playing, find another one */
			if (curplayingidx)
			{
				ssis[curplayingidx].active=0;
				ssis[curplayingidx].play=0;
				if (verbose>0) wprintw(statuswin,"STOP PLAYING %i\n",curplayingidx);
				updidx(curplayingidx);
				curplayingidx=0;
				findtoplay(curplayingidx+1);
				ref=1;
			}
			break;
		case 'F':
			wprintw(statuswin,"Filter: ");
			wgetnstr(statuswin,ssi_filter,sizeof(ssi_filter)-1);
			updopis();
			break;
		case 'f':
			switch(use_filter)
			{
				case 0: use_filter=1; break;
				case 1: use_filter=-1; break;
				case -1: use_filter=0; break;
			}
			updopis();
			break;
		case 't':
			display_state++;
			if (display_state==DISPLAY_END) display_state=DISPLAY_IDX;
			switch(display_state) {
				case DISPLAY_IDX:
					displayedwin=mainwin;
					display_mainwin();
					break;
				case DISPLAY_FREQ:
					displayedwin=freqwin;
					display_freq();
					break;
				default:
					break;
			}
			ref=1;

			break;
		case 'z':
			clear_all_freqtable();
			clear_all_receivers();
			clear_locations();
			if (displayedwin==freqwin) display_freq();
			wprintw(statuswin,"cleared frequency and location info\n");
			ref=1;
			break;
		case '?':
			wprintw(statuswin,"HELP: ");
			wprintw(statuswin,"m-mutessi  M-mute   R-record   a-alldump  ");
			wprintw(statuswin,"r-refresh  s-stop play  l-log v/V-less/more verbose\n");
			wprintw(statuswin,"f-enable/disable/invert filter F-enter filter t-toggle windows\n");
			wprintw(statuswin,"z-forget learned info\n");
			ref=1;
			break;
		default:
			wprintw(statuswin,"unknown key [%c] 0x%2.2x\n",r,r);
			ref=1;
	}
}

/*************** message parsing ****************/

char *getptr(char *s,char *id)
{
	char *c;
	c=strstr(s,id);
	if (c) return(c+strlen(id));
	return(0);
}

int getptrint(char *s,char *id,int base)
{
	char *c;
	c=getptr(s,id);
	if (!c) return(0); /* albo moze -1 byloby lepiej? */
	return(strtol(c, (char **) NULL, base));
}

int cmpfunc(char *c,char *func)
{
	if (!c) return(0);
	if (strncmp(c,func,strlen(func))==0) return(1);
	return(0);
}

int parsestat(char *c)
{
	char *func;
	char *t,*lonptr,*latptr;
	int idtype=0;
	int ssi=0;
	int usage=0;
	int encr=0;
	int writeflag=1;

	char tmpstr[BUFLEN*2];
	char tmpstr2[BUFLEN*2];
	time_t tp;
	uint16_t tmpmcc;
	uint16_t tmpmnc;
	uint16_t tmpla;
	uint8_t tmpcolour_code;
	uint32_t tmpdlf,tmpulf;
	int rxid;
	int callingssi,calledssi;
	char *sdsbegin;
	time_t tmptime;
	float longtitude,lattitude;

	func=getptr(c,"FUNC:");
	idtype=getptrint(c,"IDT:",10);
	ssi=getptrint(c,"SSI:",10);
	usage=getptrint(c,"IDX:",10);
	encr=getptrint(c,"ENCR:",10);
	rxid=getptrint(c,"RX:",10);

	if (cmpfunc(func,"BURST")) {
		if (!last_burst) {
			last_burst=10;
			updopis();
			wprintw(statuswin,"Signal found\n");
		} else {
			last_burst=10;
		}
		/* never log bursts */
		return(0);
	}

	if (cmpfunc(func,"AFCVAL")) {
		update_receivers(rxid,getptrint(c,"AFC:",10),0);
		/* never log afc values */
		return(0);
	}

	if (cmpfunc(func,"NETINFO")) {
		writeflag=0;
		tmpmnc=getptrint(c,"MNC:",16);
		tmpmcc=getptrint(c,"MCC:",16);
		tmpcolour_code=getptrint(c,"CCODE:",16);
		tmpdlf=getptrint(c,"DLF:",10);
		tmpulf=getptrint(c,"ULF:",10);
		tmpla=getptrint(c,"LA:",10);
		insert_freq(REASON_NETINFO,tmpmnc,tmpmcc,tmpulf,tmpdlf,tmpla,rxid);
		if ((tmpmnc!=netinfo.mnc)||(tmpmcc!=netinfo.mcc)||(tmpcolour_code!=netinfo.colour_code)||(tmpdlf!=netinfo.dl_freq)||(tmpulf!=netinfo.ul_freq)||(tmpla!=netinfo.la))
		{
			netinfo.mnc=tmpmnc;
			netinfo.mcc=tmpmcc;
			netinfo.colour_code=tmpcolour_code;
			netinfo.dl_freq=tmpdlf;
			netinfo.ul_freq=tmpulf;
			netinfo.la=tmpla;
			updopis();
			tmptime=time(0);

			if (netinfo.last_change==tmptime) { netinfo.changes++; } else { netinfo.changes=0; }
			if (netinfo.changes>10) {
				wprintw(statuswin,"Too much changes. Are you monitoring only one cell? (enable alldump to see)\n");
				ref=1;
			}
			netinfo.last_change=tmptime;
		}
	}
	if (cmpfunc(func,"FREQINFO1")) {
		writeflag=0;
		tmpmnc=getptrint(c,"MNC:",16);
		tmpmcc=getptrint(c,"MCC:",16);
		tmpdlf=getptrint(c,"DLF:",10);
		tmpulf=getptrint(c,"ULF:",10);
		tmpla=getptrint(c,"LA:",10);
		insert_freq(REASON_FREQINFO,tmpmnc,tmpmcc,tmpulf,tmpdlf,tmpla,rxid);

	}
	if (cmpfunc(func,"FREQINFO2")) {
		writeflag=0;
		tmpmnc=getptrint(c,"MNC:",16);
		tmpmcc=getptrint(c,"MCC:",16);
		tmpdlf=getptrint(c,"DLF:",10);
		tmpulf=getptrint(c,"ULF:",10);
		tmpla=getptrint(c,"LA:",10);
		insert_freq(REASON_DLFREQ,tmpmnc,tmpmcc,tmpulf,tmpdlf,tmpla,rxid);

	}
	if (cmpfunc(func,"DSETUPDEC"))
	{
		//addssi2(usage,ssi,0);
		addssi(usage,ssi);
		updidx(usage);

	}
	if (cmpfunc(func,"SDSDEC")) {
		callingssi=getptrint(c,"CallingSSI:",10);
		calledssi=getptrint(c,"CalledSSI:",10);
		sdsbegin=strstr(c,"DATA:");
		latptr=getptr(c," lat:");
		lonptr=getptr(c," lon:");
		if ((strstr(c,"Text")))
		{
			wprintw(statuswin,"SDS %i->%i %s\n",callingssi,calledssi,sdsbegin);
			ref=1;


		}
		/* handle location */
		if ((kml_tmp_file)&&(strstr(c,"INVALID_POSITION")==0)&&(latptr)&&(lonptr))
		{
			lattitude=atof(latptr);
			longtitude=atof(lonptr);
			t=latptr;
			while ((*t)&&(*t!=' ')) {
				if (*t=='S') { lattitude=-lattitude; break; }
				t++;
			}
			t=lonptr;
			while ((*t)&&(*t!=' ')) {
				if (*t=='W') { longtitude=-longtitude; break; }
				t++;
			}
			add_location(callingssi,lattitude,longtitude,c);

		}
	}
	if (idtype==ADDR_TYPE_SSI_USAGE) {
		if (cmpfunc(func,"D-SETUP"))
		{
			//addssi2(usage,ssi,0);
			addssi(usage,ssi);
			updidx(usage);
		}
		if (cmpfunc(func,"D-CONNECT"))
		{
			//addssi2(usage,ssi,1);
			addssi(usage,ssi);
			updidx(usage);
		}

	}
	if (cmpfunc(func,"D-RELEASE"))
	{
		/* don't use releasessi for now, as we can have the same ssi
		 * on different usage identifiers. one day this should be
		 * done properly with notif. ids */
		//		releasessi(ssi);
	}


	if (alldump) writeflag=1;
	if ((writeflag)&&(strcmp(c,prevtmsg)))
	{
		tp=time(0);
		strftime(tmpstr,40,"%Y%m%d %H:%M:%S",localtime(&tp));

		snprintf(tmpstr2,sizeof(tmpstr2)-1,"%s %s",tmpstr,c);

		wprintw(msgwin,"%s\n",tmpstr2);
		strncpy(prevtmsg,c,sizeof(prevtmsg)-1);
		prevtmsg[sizeof(prevtmsg)-1]=0;
		if (do_log) appendlog(tmpstr2);
	}

	return(0);

}


int parsetraffic(unsigned char *buf)
{
	unsigned char *c;
	int usage;
	int len=1380;
	time_t tt=time(0);
	FILE *f;
	int rxid;
	usage=getptrint((char *)buf,"TRA",16);
	rxid=getptrint((char *)buf,"RX",16);
	if ((usage<1)||(usage>63)) return(0);
	c=buf+6;

	if (!ssis[usage].active) {
		ssis[usage].active=1;
		updidx(usage);
	}
	ssis[usage].timeout=tt;

	if ((strncmp((char *)buf,"TRA",3)==0)&&(!ssis[usage].encr)) {
		if ((mutessi)&&(!ssis[usage].ssi[0])&&(!ssis[usage].ssi[1])&&(!ssis[usage].ssi[2])) return(0); /* ignore it if we don't know any ssi for this usage identifier */

		findtoplay(0);
		if ((curplayingidx)&&(curplayingidx==usage)) {
			if (!matchidx(usage)) {
				ssis[curplayingidx].active=0;
				ssis[curplayingidx].play=0;
				if (verbose>0) wprintw(statuswin,"STOP PLAYING %i\n",curplayingidx);
				updidx(curplayingidx);
				curplayingidx=0;
				findtoplay(curplayingidx+1);
				ref=1;
				return(0);
			}
			ssis[usage].play=1;
			if (!ps_mute)	{
				if (!ferror(playingfp)) {
					fwrite(c,1,len,playingfp);
					fflush(playingfp);
				} else {
					wprintw(statuswin,"PLAYBACK PROBLEM!! (fix tplay)\n");
					playingfp=NULL;
					curplayingidx=0;
				}
			}
			curplayingtime=time(0);
			curplayingticks=0;
			updidx(usage);
			ref=1;
		}
		if ((strlen(ssis[usage].curfile)==0)||(ssis[usage].ssi_time_rec+rec_timeout<tt)) {
			/* either it has no name, or there was a timeout,
			 * change the file name */
			strftime(ssis[usage].curfiletime,32,"%Y%m%d_%H%M%S",localtime(&tt));
			sprintf(ssis[usage].curfile,"%s/traffic_%i.tmp",outdir,usage);
			if (verbose>1) wprintw(statuswin,"newfile %s\n",ssis[usage].curfile);
			ref=1;
		}
		if (strlen(ssis[usage].curfile))
		{
			if (ps_record) {
				f=fopen(ssis[usage].curfile,"ab");
				if (f) {
					fwrite(c, 1, len, f);
					fclose(f);
				}
			}
			ssis[usage].ssi_time_rec=tt;
		}


	}

	return(0);
}

/* get config from env variables, maybe i should switch it to getopt() one day */
void get_cfgenv() {

	if (getenv("TETRA_OUTDIR"))
	{
		outdir=getenv("TETRA_OUTDIR");
	} else {
		outdir=def_outdir;
	}


	if (getenv("TETRA_LOGFILE"))
	{
		logfile=getenv("TETRA_LOGFILE");
	} else {
		logfile=def_logfile;
	}
	if (getenv("TETRA_SSI_FILTER"))
	{
		strncpy((char *)&ssi_filter,getenv("TETRA_SSI_FILTER"),sizeof(ssi_filter)-1);
	} else {
		ssi_filter[0]=0;
	}

	if (getenv("TETRA_SSI_DESCRIPTIONS"))
	{
		ssifile=getenv("TETRA_SSI_DESCRIPTIONS");
	} else {
		ssifile=def_ssifile;
	}

	if (getenv("TETRA_KML_FILE"))
	{
		kml_file=getenv("TETRA_KML_FILE");
		kml_tmp_file=malloc(strlen(kml_file)+6);
		sprintf(kml_tmp_file,"%s.tmp",kml_file);
		if (getenv("TETRA_KML_INTERVAL")) {
			kml_interval=atoi(getenv("TETRA_KML_INTERVAL"));
		} else {
			kml_interval=30;
		}
	} else {
		kml_interval=0;
	}

	if (getenv("TETRA_LOCK_FILE"))
	{
		lock_file=getenv("TETRA_LOCK_FILE");
		lockfd=open(lock_file,O_RDWR|O_CREAT,0600);
		if (lockfd<1) lockfd=0;
	}

	if (getenv("TETRA_REC_TIMEOUT")) rec_timeout=atoi(getenv("TETRA_REC_TIMEOUT"));
	if (getenv("TETRA_SSI_TIMEOUT")) ssi_timeout=atoi(getenv("TETRA_SSI_TIMEOUT"));
	if (getenv("TETRA_IDX_TIMEOUT")) idx_timeout=atoi(getenv("TETRA_IDX_TIMEOUT"));
	if (getenv("TETRA_CURPLAYING_TIMEOUT")) curplaying_timeout=atoi(getenv("TETRA_CURPLAYING_TIMEOUT"));
	if (getenv("TETRA_FREQ_TIMEOUT")) freq_timeout=atoi(getenv("TETRA_FREQ_TIMEOUT"));

}

int main(void)
{
	struct sockaddr_in si_me, si_other;
	int s;
	socklen_t slen=sizeof(si_other);
	unsigned char buf[BUFLEN];
	char *c,*d;
	int len;
	int tport;

    telive_zmq_init();

	//system("resize -s 60 203"); /* this blocks on some xterms, no idea why */
	if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1)
		diep("socket");

	get_cfgenv();
	if (getenv("TETRA_PORT"))
	{
		tport=atoi(getenv("TETRA_PORT"));
	} else {
		tport=PORT;
	}

	memset((char *) &si_me, 0, sizeof(si_me));
	si_me.sin_family = AF_INET;
	si_me.sin_port = htons(tport);
	si_me.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr *)&si_me, sizeof(si_me))==-1)
		diep("bind");


	initcur();
	updopis();
	do_popen();

	ref=0;

	if (getenv("TETRA_KEYS")) {
		c=getenv("TETRA_KEYS");
		while(*c) {
			keyf(*c);
			c++;
		}
	}

	signal(SIGPIPE,SIG_IGN);

	fd_set rfds;
	int nfds;
	int r;
	struct timeval timeout;

	while (1) {
		FD_ZERO(&rfds);
		FD_SET(s,&rfds);
		FD_SET(0,&rfds);
		nfds=max(0,s+1);
		timeout.tv_sec=0;
		timeout.tv_usec=50*1000; //50ms
		r=select(nfds,&rfds,0,0,&timeout);

		//timeout
		if (r==0) tickf();

		if (r==-1) {
			wprintw(statuswin,"select ret -1\n");
			wrefresh (statuswin);
		}

		if ((r>0)&&(FD_ISSET(0,&rfds)))
		{
			len=read(0,buf,1);

			if (len==1) keyf(buf[0]);
		}

		if ((r>0)&&(FD_ISSET(s,&rfds)))
		{
			bzero(buf,BUFLEN);
			len=recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *)&si_other, &slen);

			if (len==-1)
				diep("recvfrom()");

			c=strstr((char *)buf,"TETMON_begin");
			if (c)
			{
				c=c+13;
				d=strstr((char *)buf,"TETMON_end");
				if (d) {
					*d=0;
					parsestat(c);
					ref=1;
				} else
				{
					wprintw(statuswin,"bad line [%80s]\n",buf);
					ref=1;
				}


			} else
			{
				if (len==1386)
				{
					if (newopis()) initopis();
					parsetraffic(buf);
				} else
				{

					wprintw(statuswin,"### SMALL FRAME: write %i\n",len);
					ref=1; }

			}
		}
		if (ref) refresh_scr();

	}
	close(s);
	return 0;
}
