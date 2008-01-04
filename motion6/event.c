/*
	event.c

	Generalised event handling for motion

	Copyright Jeroen Vreeken, 2002
	This software is distributed under the GNU Public License Version 2
	see also the file 'COPYING'.

*/

#include "motion.h"
#include "event.h"

#ifdef __freebsd__
#include "video_freebsd.h"
#else
#include "video.h"
#endif /* __freebsd__ */

#include "picture.h"
#include "ffmpeg.h"

/*
 *	Various functions (most doing the actual action)
 */

/* Execute 'command' with 'arg' as its argument.
 * if !arg command is started with no arguments
 * Before we call execl we need to close all the file handles
 * that the fork inherited from the parent in order not to pass
 * the open handles on to the shell
 */
static void exec_command(struct context *cnt, char *command, char *arg)
{
	char stamp[PATH_MAX];
	mystrftime(cnt ,stamp, sizeof(stamp), command, cnt->currenttime);
	if (arg) {
		strcat(stamp, " ");
		strcat(stamp, arg);
	}
	
	if (!fork()) {
		int i;
		
		/* Detach from parent */
		setsid();

		/* Close any file descripter except console because we will like to see error messages */
		for (i=getdtablesize(); i>2; --i)
			close(i);
		
		execl("/bin/sh", "sh", "-c", stamp, " &", NULL);

		/* if above function succeeds the program never reach here */
		motion_log(cnt, LOG_ERR, 1, "Unable to start external command '%s' with parameters '%s'",stamp,arg);

		exit(1);
	}
	else if (cnt->conf.setup_mode)
		motion_log(cnt, -1, 0, "Executing external command '%s'", stamp);
}

/* 
 *	Event handlers
 */

static void event_newfile(struct context *cnt, int type, unsigned char *dummy,
                          char *filename, void *ftype, struct tm *tm)
{
	motion_log(cnt, -1, 0, "File of type %ld saved to: %s", (unsigned long)ftype, filename);
}

static void event_motion(struct context *cnt, int type, unsigned char *dummy,
                         char *filename, void *ftype, struct tm *tm)
{
	if (!cnt->conf.quiet)
		printf("\a");
}

static void on_picture_save_command(struct context *cnt, int type, unsigned char *dummy,
                                    char *filename, void *arg, struct tm *tm)
{
	int ftype = (unsigned long)arg;	

	if ((ftype & FTYPE_IMAGE_ANY) != 0 && cnt->conf.on_picture_save)
		exec_command(cnt, cnt->conf.on_picture_save, filename);

	if ((ftype & FTYPE_MPEG_ANY) != 0 && cnt->conf.on_movie_start)
		exec_command(cnt, cnt->conf.on_movie_start, filename);
}

static void on_motion_detected_command(struct context *cnt, int type, unsigned char *dummy1,
                                       char *dummy2, void *dummy3, struct tm *tm)
{
	if (cnt->conf.on_motion_detected)
		exec_command(cnt, cnt->conf.on_motion_detected, 0);
}

#if defined(HAVE_MYSQL) || defined(HAVE_PGSQL)

static void event_sqlnewfile(struct context *cnt, int type, unsigned char *dummy,
                             char *filename, void *arg, struct tm *tm)
{
	int sqltype = (unsigned long)arg;

	/* Only log the file types we want */
	if (!(cnt->conf.mysql_db || cnt->conf.pgsql_db) || (sqltype & cnt->sql_mask) == 0) 
		return;

	/* We place the code in a block so we only spend time making space in memory
	 * for the sqlquery and timestr when we actually need it.
	 */
	{
		char sqlquery[512];
		char timestr[20];
	
		strftime(timestr, sizeof(timestr), "%Y-%m-%d %T", cnt->currenttime);
	
		sprintf(sqlquery,
		    "insert into security(camera, filename, frame, file_type, "
		    "time_stamp %s) values('%d', '%s', '%d', '%d', '%s' %s%s%s)",
		    cnt->conf.text_left ? ", text_left" : "" ,
		    cnt->threadnr, filename, cnt->shots, sqltype,  timestr,
		    cnt->conf.text_left ? ", '" : "" ,
		    cnt->conf.text_left ? cnt->conf.text_left : "" ,
		    cnt->conf.text_left ? "'" : ""
		    );

#ifdef HAVE_MYSQL
		if (cnt->conf.mysql_db) {
			if (mysql_query(cnt->database, sqlquery) != 0)
				motion_log(cnt, LOG_ERR, 1, "Mysql query failed");
		}
#endif /* HAVE_MYSQL */

#ifdef HAVE_PGSQL
		if (cnt->conf.pgsql_db) {
			PGresult *res;

			res = PQexec(cnt->database_pg, sqlquery);

			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				motion_log(cnt, LOG_ERR, 1, "PGSQL query failed");
				PQclear(res);
			}
		}
#endif /* HAVE_PGSQL */

	}
}

#endif /* defined HAVE_MYSQL || defined HAVE_PGSQL */


static void event_firstmotion(struct context *cnt, int type, unsigned char *dummy1,
                              char *dummy2, void *dummy3, struct tm *tm)
{
	if (cnt->conf.on_event_start)
		exec_command(cnt, cnt->conf.on_event_start, 0);
}

static void on_event_end_command(struct context *cnt, int type, unsigned char *dummy1,
                                 char *dummy2, void *dummy3, struct tm *tm)
{
	if (cnt->conf.on_event_end)
		exec_command(cnt, cnt->conf.on_event_end, 0);
}

static void event_stop_webcam(struct context *cnt, int type, unsigned char *dummy1,
                              char *dummy2, void *dummy3, struct tm *tm)
{
	if (cnt->conf.webcam_port){
		webcam_stop(cnt);
	}
}

static void event_webcam_put(struct context *cnt, int type, unsigned char *img,
                             char *dummy1, void *dummy2, struct tm *tm)
{
	if (cnt->conf.webcam_port) {
		webcam_put(cnt, img);
	}
}

#ifndef WITHOUT_V4L
#ifndef __freebsd__
static void event_vid_putpipe(struct context *cnt, int type, unsigned char *img,
                              char *dummy, void *devpipe, struct tm *tm)
{
	if (*(int *)devpipe >= 0) {
		if (vid_putpipe(*(int *)devpipe, img, cnt->imgs.size) == -1)
			motion_log(cnt, LOG_ERR, 1, "Failed to put image into video pipe");
	}
}
#endif /* __freebsd__ */
#endif /* WITHOUT_V4L */


char *imageext(struct context *cnt)
{
	if (cnt->conf.ppm)
		return "ppm";
	return "jpg";
}

static void event_image_detect(struct context *cnt, int type, unsigned char *newimg,
                               char *dummy1, void *dummy2, struct tm *currenttime)
{
	struct config *conf=&cnt->conf;
	char fullfilename[PATH_MAX];
	char filename[PATH_MAX];
	char fullfilenamem[PATH_MAX];
	char filenamem[PATH_MAX];

	if (conf->motion_img || cnt->new_img==NEWIMG_ON || cnt->preview_shot) {
		char *jpegpath;

		/* conf.jpegpath would normally be defined but if someone deleted it by control interface
		   it is better to revert to the default than fail */
		if (cnt->conf.jpegpath)
			jpegpath = cnt->conf.jpegpath;
		else
			jpegpath = DEF_JPEGPATH;
			
		mystrftime(cnt, filename, sizeof(filename), jpegpath, currenttime);
		/* motion images gets same name as normal images plus an appended 'm' */
		sprintf(filenamem, "%sm", filename);
		sprintf(fullfilename, "%s/%s.%s", cnt->conf.filepath, filename, imageext(cnt));
		sprintf(fullfilenamem, "%s/%s.%s", cnt->conf.filepath, filenamem, imageext(cnt));
	}
	if (conf->motion_img) {
		put_picture(cnt, fullfilenamem, cnt->imgs.out, FTYPE_IMAGE_MOTION);
	}
	if (cnt->new_img==NEWIMG_ON || cnt->preview_shot) {
		put_picture(cnt, fullfilename, newimg, FTYPE_IMAGE);
	}
}

static void event_image_snapshot(struct context *cnt, int type, unsigned char *img,
                                 char *dummy1, void *dummy2, struct tm *currenttime)
{
	char fullfilename[PATH_MAX];

	if ( strcmp(cnt->conf.snappath, "lastsnap") ) {
		char filename[PATH_MAX];
		char filepath[PATH_MAX];
		char linkpath[PATH_MAX];
		char *snappath;
		/* conf.snappath would normally be defined but if someone deleted it by control interface
		   it is better to revert to the default than fail */
		if (cnt->conf.snappath)
			snappath = cnt->conf.snappath;
		else
			snappath = DEF_SNAPPATH;
			
		mystrftime(cnt, filepath, sizeof(filepath), snappath, currenttime);
		sprintf(filename, "%s.%s", filepath, imageext(cnt));
		sprintf(fullfilename, "%s/%s", cnt->conf.filepath, filename);
		put_picture(cnt, fullfilename, img, FTYPE_IMAGE_SNAPSHOT);

		/* Update symbolic link *after* image has been written so that
		   the link always points to a valid file. */
		sprintf(linkpath, "%s/lastsnap.%s", cnt->conf.filepath, imageext(cnt));
		remove(linkpath);
		if (symlink(filename, linkpath)) {
			motion_log(cnt, LOG_ERR, 1, "Could not create symbolic link [%s]",filename);
			return;
		}
	} else {
		sprintf(fullfilename, "%s/lastsnap.%s", cnt->conf.filepath, imageext(cnt));
		remove(fullfilename);
		put_picture(cnt, fullfilename, img, FTYPE_IMAGE_SNAPSHOT);
	}

	cnt->snapshot=0;
}

#ifdef HAVE_FFMPEG
static void grey2yuv420p(unsigned char *u, unsigned char *v, int width, int height)
{
	memset(u, 128, width*height/4);
	memset(v, 128, width*height/4);
}

static void on_movie_end_command(struct context *cnt, int type, unsigned char *dummy,
                                 char *filename, void *arg, struct tm *tm)
{
	int ftype = (unsigned long) arg;

	if ((ftype & FTYPE_MPEG_ANY) && cnt->conf.on_movie_end)
		exec_command(cnt, cnt->conf.on_movie_end, filename);
}

static void event_ffmpeg_newfile(struct context *cnt, int type, unsigned char *img,
                                 char *dummy1, void *dummy2, struct tm *currenttime)
{
	int width=cnt->imgs.width;
	int height=cnt->imgs.height;
	unsigned char *convbuf, *y, *u, *v;
	int fps;
	char stamp[PATH_MAX];
	char *mpegpath;

	if (!cnt->conf.ffmpeg_cap_new && !cnt->conf.ffmpeg_cap_motion)
		return;
		
	/* conf.mpegpath would normally be defined but if someone deleted it by control interface
	   it is better to revert to the default than fail */
	if (cnt->conf.mpegpath)
		mpegpath = cnt->conf.mpegpath;
	else
		mpegpath = DEF_MPEGPATH;
	mystrftime(cnt, stamp, sizeof(stamp), mpegpath, currenttime);
	/* motion mpegs get the same name as normal mpegs plus an appended 'm' */
	sprintf(cnt->motionfilename, "%s/%sm", cnt->conf.filepath, stamp);
	sprintf(cnt->newfilename, "%s/%s", cnt->conf.filepath, stamp);

	if (cnt->conf.ffmpeg_cap_new) {
		if (cnt->imgs.type==VIDEO_PALETTE_GREY) {
			convbuf=mymalloc(cnt, (width*height)/2);
			y=img;
			u=convbuf;
			v=convbuf+(width*height)/4;
			grey2yuv420p(u, v, width, height);
		} else {
			convbuf=NULL;
			y=img;
			u=img+width*height;
			v=u+(width*height)/4;
		}
		if (cnt->conf.low_cpu)
			fps=cnt->conf.frame_limit;
		else
			fps=cnt->lastrate;
		if (fps>30)
			fps=30;
		if (fps<2)
			fps=2;
		if ( (cnt->ffmpeg_new =
		      ffmpeg_open(cnt, cnt->conf.ffmpeg_video_codec, cnt->newfilename, y, u, v,
		                  cnt->imgs.width, cnt->imgs.height, fps, cnt->conf.ffmpeg_bps,
		                  cnt->conf.ffmpeg_vbr)) == NULL) {
			motion_log(cnt, LOG_ERR, 1, "ffopen_open error creating file [%s]",cnt->newfilename);
			cnt->finish=1;
			return;
		}
		((struct ffmpeg *)cnt->ffmpeg_new)->udata=convbuf;
		event(cnt, EVENT_FILECREATE, NULL, cnt->newfilename, (void *)FTYPE_MPEG, NULL);
	}
	if (cnt->conf.ffmpeg_cap_motion) {
		if (cnt->imgs.type==VIDEO_PALETTE_GREY) {
			convbuf=mymalloc(cnt, (width*height)/2);
			y=cnt->imgs.out;
			u=convbuf;
			v=convbuf+(width*height)/4;
			grey2yuv420p(u, v, width, height);
		} else {
			y=cnt->imgs.out;
			u=cnt->imgs.out+width*height;
			v=u+(width*height)/4;
			convbuf=NULL;
		}
		if (cnt->conf.low_cpu)
			fps=cnt->conf.frame_limit;
		else
			fps=cnt->lastrate;
		if (fps>30)
			fps=30;
		if (fps<2)
			fps=2;
		if ( (cnt->ffmpeg_motion =
		      ffmpeg_open(cnt ,cnt->conf.ffmpeg_video_codec, cnt->motionfilename, y, u, v,
		                  cnt->imgs.width, cnt->imgs.height, fps, cnt->conf.ffmpeg_bps,
		                  cnt->conf.ffmpeg_vbr)) == NULL){
			motion_log(cnt, LOG_ERR, 1, "ffopen_open error creating file [%s]", cnt->motionfilename);
			cnt->finish=1;
			return;
		}
		cnt->ffmpeg_motion->udata=convbuf;
		event(cnt, EVENT_FILECREATE, NULL, cnt->motionfilename, (void *)FTYPE_MPEG_MOTION, NULL);
	}
}

static void event_ffmpeg_timelapse(struct context *cnt, int type, unsigned char *img,
                                   char *dummy1, void *dummy2, struct tm *currenttime)
{
	int width = cnt->imgs.width;
	int height = cnt->imgs.height;
	unsigned char *convbuf, *y, *u, *v;

	if (!cnt->ffmpeg_timelapse) {
		char tmp[PATH_MAX], *timepath;

		/* conf.timepath would normally be defined but if someone deleted it by control interface
		   it is better to revert to the default than fail */
		if (cnt->conf.timepath)
			timepath = cnt->conf.timepath;
		else
			timepath = DEF_TIMEPATH;
		
		mystrftime(cnt, tmp, sizeof(tmp), timepath, currenttime);
		sprintf(cnt->timelapsefilename, "%s/%s", cnt->conf.filepath, tmp);
		if (cnt->imgs.type == VIDEO_PALETTE_GREY) {
			convbuf = mymalloc(cnt, (width*height)/2);
			y = img;
			u = convbuf;
			v = convbuf+(width*height)/4;
			grey2yuv420p(u, v, width, height);
		} else {
			convbuf = NULL;
			y = img;
			u = img+width*height;
			v = u+(width*height)/4;
		}
		if ( (cnt->ffmpeg_timelapse =
		      ffmpeg_open(cnt ,TIMELAPSE_CODEC, cnt->timelapsefilename, y, u, v,
		                  cnt->imgs.width, cnt->imgs.height, 24, cnt->conf.ffmpeg_bps,
		                  cnt->conf.ffmpeg_vbr)) == NULL) {
			motion_log(cnt, LOG_ERR, 1, "ffopen_open error creating file [%s]", cnt->timelapsefilename);
			cnt->finish=1;
			return;
		}
		cnt->ffmpeg_timelapse->udata = convbuf;
		event(cnt, EVENT_FILECREATE, NULL, cnt->timelapsefilename, (void *)FTYPE_MPEG_TIMELAPSE, NULL);
	}
	
	y = img;
	
	if (cnt->imgs.type == VIDEO_PALETTE_GREY)
		u = cnt->ffmpeg_timelapse->udata;
	else
		u = img+width*height;
	
	v = u+(width*height)/4;
	ffmpeg_put_other_image(cnt ,cnt->ffmpeg_timelapse, y, u, v);
	
}

static void event_ffmpeg_put(struct context *cnt, int type, unsigned char *img,
                             char *dummy1, void *dummy2, struct tm *tm)
{
	if (cnt->ffmpeg_new)
	{
		int width=cnt->imgs.width;
		int height=cnt->imgs.height;
		unsigned char *y = img;
		unsigned char *u, *v;
		
		if (cnt->imgs.type == VIDEO_PALETTE_GREY)
			u = cnt->ffmpeg_timelapse->udata;
		else
			u = y + (width * height);
		
		v = u + (width * height) / 4;
		ffmpeg_put_other_image(cnt, cnt->ffmpeg_new, y, u, v);
	}
	
	if (cnt->ffmpeg_motion) {
		ffmpeg_put_image(cnt, cnt->ffmpeg_motion);
	}
}

static void event_ffmpeg_closefile(struct context *cnt, int type, unsigned char *dummy1,
                                   char *dummy2, void *dummy3, struct tm *tm)
{
	
	if (cnt->ffmpeg_new) {
		if (cnt->ffmpeg_new->udata)
			free(cnt->ffmpeg_new->udata);
		ffmpeg_close(cnt->ffmpeg_new);
		cnt->ffmpeg_new=NULL;

		event(cnt, EVENT_FILECLOSE, NULL, cnt->newfilename, (void *)FTYPE_MPEG, NULL);
	}
	if (cnt->ffmpeg_motion) {
		if (cnt->ffmpeg_motion->udata)
			free(cnt->ffmpeg_motion->udata);
		ffmpeg_close(cnt->ffmpeg_motion);
		cnt->ffmpeg_motion=NULL;

		event(cnt, EVENT_FILECLOSE, NULL, cnt->motionfilename, (void *)FTYPE_MPEG_MOTION, NULL);
	}
}

static void event_ffmpeg_timelapseend(struct context *cnt, int type, unsigned char *dummy1,
                                      char *dummy2, void *dummy3, struct tm *tm)
{
	if (cnt->ffmpeg_timelapse) {
		if (cnt->ffmpeg_timelapse->udata)
			free(cnt->ffmpeg_timelapse->udata);
		ffmpeg_close(cnt->ffmpeg_timelapse);
		cnt->ffmpeg_timelapse=NULL;

		event(cnt, EVENT_FILECLOSE, NULL, cnt->timelapsefilename, (void *)FTYPE_MPEG_TIMELAPSE, NULL);
	}
}

#endif /* HAVE_FFMPEG */


/*  
 *	Starting point for all events
 */

struct event_handlers {
	int type;
	event_handler handler;
};

struct event_handlers event_handlers[] = {
	{
	EVENT_FILECREATE,
	event_newfile
	},
	{
	EVENT_FILECREATE,
	on_picture_save_command
	},
#if defined(HAVE_MYSQL) || defined(HAVE_PGSQL) 
	{
	EVENT_FILECREATE,
	event_sqlnewfile
	},
#endif
	{
	EVENT_MOTION,
	event_motion
	},
	{
	EVENT_MOTION,
	on_motion_detected_command
	},
	{
	EVENT_FIRSTMOTION,
	event_firstmotion
	},
	{
	EVENT_ENDMOTION,
	on_event_end_command
	},
	{
	EVENT_IMAGE_DETECTED,
	event_image_detect
	},
	{
	EVENT_IMAGE_SNAPSHOT,
	event_image_snapshot
	},
#ifndef WITHOUT_V4L
#ifndef __freebsd__
	{
	EVENT_IMAGE | EVENT_IMAGEM,
	event_vid_putpipe
	},
#endif /* __freebsd__ */
#endif /* WITHOUT_V4L */
	{
	EVENT_WEBCAM,
	event_webcam_put
	},
#ifdef HAVE_FFMPEG
	{
	EVENT_FIRSTMOTION,
	event_ffmpeg_newfile
	},
	{
	EVENT_IMAGE_DETECTED,
	event_ffmpeg_put
	},
	{
	EVENT_ENDMOTION,
	event_ffmpeg_closefile
	},
	{
	EVENT_TIMELAPSE,
	event_ffmpeg_timelapse
	},
	{
	EVENT_TIMELAPSEEND,
	event_ffmpeg_timelapseend
	},
	{
	EVENT_FILECLOSE,
	on_movie_end_command
	},
#endif /* HAVE_FFMPEG */
	{
	EVENT_STOP,
	event_stop_webcam
	},
	{0, NULL}
};


/* The event functions are defined with the following parameters:
 * - Type as defined in event.h (EVENT_...)
 * - The global context struct cnt
 * - image - A pointer to unsigned char as used for images
 * - filename - A pointer to typically a string for a file path
 * - eventdata - A void pointer that can be cast to anything. E.g. FTYPE_...
 * - tm - A tm struct that carries a full time structure
 * The split between unsigned images and signed filenames was introduced in 3.2.2
 * as a code reading friendly solution to avoid a stream of compiler warnings in gcc 4.0.
 */
void event(struct context *cnt, int type, unsigned char *image, char *filename, void *eventdata, struct tm *tm)
{
	int i=-1;

	while (event_handlers[++i].handler) {
		if (type & event_handlers[i].type)
			event_handlers[i].handler(cnt, type, image, filename, eventdata, tm);
	}
}