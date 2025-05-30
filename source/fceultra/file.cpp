/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Xodnizel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <sys/types.h>
#include <sys/stat.h>
#include <fstream>

#include "types.h"
#include "file.h"
#include "utils/endian.h"
#include "utils/memory.h"
#include "utils/md5.h"
#ifdef GEKKO
#include "utils/unzip/unzip.h"
#else
#include "utils/unzip.h"
#endif
#include "driver.h"
#include "types.h"
#include "fceu.h"
#include "state.h"
#include "movie.h"
#include "driver.h"
#include "utils/xstring.h"

#ifndef WIN32
#include <zlib.h>
#endif

using namespace std;

bool bindSavestate = true;	//Toggle that determines if a savestate filename will include the movie filename
static std::string BaseDirectory;
static char FileExt[2048];	//Includes the . character, as in ".nes"
char FileBase[2048];
static char FileBaseDirectory[2048];


void ApplyIPS(FILE *ips, FCEUFILE* fp)
{
	uint8 header[5];
	uint32 count=0;

	if(!ips) return;

	char* buf = (char*)FCEU_malloc(fp->size);
	memcpy(buf,fp->EnsureMemorystream()->buf(),fp->size);


	FCEU_printf(" Applying IPS...\n");
	if(fread(header,1,5,ips)!=5)
	{
		goto end;
	}
	if(memcmp(header,"PATCH",5))
	{
		goto end;
	}

	while(fread(header,1,3,ips)==3)
	{
		uint32 offset=(header[0]<<16)|(header[1]<<8)|header[2];
		uint16 size;

		if(!memcmp(header,"EOF",3))
		{
			FCEU_printf(" IPS EOF:  Did %d patches\n\n",count);
			goto end;
		}

		size=fgetc(ips)<<8;
		size|=fgetc(ips);
		if(!size)	/* RLE */
		{
			char *start;
			char b;
			size=fgetc(ips)<<8;
			size|=fgetc(ips);

			//FCEU_printf("  Offset: %8d  Size: %5d RLE\n",offset,size);

			if((offset+size)>(uint32)fp->size)
			{
				// Probably a little slow.
				char *newbuf=(char *)FCEU_realloc(buf,offset+size);
				buf=newbuf;
				memset(buf+fp->size,0,offset+size-fp->size);
				fp->size=offset+size;
			}
			b=fgetc(ips);
			start=buf+offset;
			do
			{
				*start=b;
				start++;
			} while(--size);
		}
		else		/* Normal patch */
		{
			//FCEU_printf("  Offset: %8d  Size: %5d\n",offset,size);
			if((offset+size)>(uint32)fp->size)
			{
				// Probably a little slow.
				char *newbuf=(char *)FCEU_realloc(buf,offset+size);
				buf=newbuf;
				memset(buf+fp->size,0,offset+size-fp->size);
				fp->size=offset+size;
			}
			if ( fread(buf+offset,1,size,ips) != static_cast<size_t>(size) )
			{
				FCEU_printf(" Warn IPS data read came up short!\n");
			}
		}
		count++;
	}
	FCEU_printf(" Hard IPS end!\n");
end:
	fclose(ips);
	EMUFILE_MEMORY* ms = new EMUFILE_MEMORY(buf,fp->size);
	fp->SetStream(ms);
}

std::string FCEU_MakeIpsFilename(FileBaseInfo fbi) {
	char ret[FILENAME_MAX] = "";
	snprintf(ret, sizeof(ret), "%s" PSS "%s%s.ips",fbi.filebasedirectory.c_str(),fbi.filebase.c_str(),fbi.ext.c_str());
	return ret;
}

void FCEU_SplitArchiveFilename(std::string src, std::string& archive, std::string& file, std::string& fileToOpen)
{
	size_t pipe = src.find_first_of('|');
	if(pipe == std::string::npos)
	{
		archive = "";
		file = src;
		fileToOpen = src;
	}
	else
	{
		archive = src.substr(0,pipe);
		file = src.substr(pipe+1);
		fileToOpen = archive;
	}
}

FileBaseInfo CurrentFileBase() {
	return FileBaseInfo(FileBaseDirectory,FileBase,FileExt);
}

FileBaseInfo DetermineFileBase(const char *f) {

	char drv[PATH_MAX], dir[PATH_MAX], name[PATH_MAX], ext[PATH_MAX];
	splitpath(f,drv,dir,name,ext);

        if(dir[0] == 0) strcpy(dir,".");

	return FileBaseInfo((std::string)drv + dir,name,ext);

}

inline FileBaseInfo DetermineFileBase(const std::string& str) { return DetermineFileBase(str.c_str()); }

static FCEUFILE * TryUnzip(const std::string& path) {
	unzFile tz;
	if((tz=unzOpen(path.c_str())))  // If it's not a zip file, use regular file handlers.
		// Assuming file type by extension usually works,
		// but I don't like it. :)
	{
		if(unzGoToFirstFile(tz)==UNZ_OK)
		{
			for(;;)
			{
				char tempu[512];	// Longer filenames might be possible, but I don't
				// think people would name files that long in zip files...
				unzGetCurrentFileInfo(tz,0,tempu,512,0,0,0,0);
				tempu[511]=0;
				if(strlen(tempu)>=4)
				{
					char *za=tempu+strlen(tempu)-4;

					//if(!ext)
					{
						if(!strcasecmp(za,".nes") || !strcasecmp(za,".fds") ||
							!strcasecmp(za,".nsf") || !strcasecmp(za,".unf") ||
							!strcasecmp(za,".nez"))
							break;
					}
					//else if(!strcasecmp(za,ext))
					//	break;
				}
				if(strlen(tempu)>=5)
				{
					if(!strcasecmp(tempu+strlen(tempu)-5,".unif"))
						break;
				}
				if(unzGoToNextFile(tz)!=UNZ_OK)
				{
					if(unzGoToFirstFile(tz)!=UNZ_OK) goto zpfail;
					unzCloseCurrentFile(tz);
					unzClose(tz);
					return 0;
				}
			}
			if(unzOpenCurrentFile(tz)!=UNZ_OK)
				goto zpfail;
		}
		else
		{
zpfail:
			unzClose(tz);
			return 0;
		}

		unz_file_info ufo;
		unzGetCurrentFileInfo(tz,&ufo,0,0,0,0,0,0);

		int size = ufo.uncompressed_size;
		EMUFILE_MEMORY* ms = new EMUFILE_MEMORY(size);
		unzReadCurrentFile(tz,ms->buf(),ufo.uncompressed_size);
		unzCloseCurrentFile(tz);
		unzClose(tz);

		FCEUFILE *fceufp = new FCEUFILE();
		fceufp->stream = ms;
		fceufp->size = size;
		return fceufp;

	}

	return 0;
}

FCEUFILE * FCEU_fopen(const char *path, const char *ipsfn, const char *mode, char *ext, int index, const char** extensions, int* userCancel)
{
	FILE *ipsfile=0;
	FCEUFILE *fceufp=0;

	bool read = !strcmp(mode, "rb");
	bool write = !strcmp(mode, "wb");
	if(read && write || !read && !write)
	{
		FCEU_PrintError("invalid file open mode specified (only wb and rb are supported)");
		return 0;
	}

	std::string archive,fname,fileToOpen;
	FCEU_SplitArchiveFilename(path,archive,fname,fileToOpen);


	//try to setup the ips file
	if(ipsfn && read)
		ipsfile=FCEUD_UTF8fopen(ipsfn,"rb");
	if(read)
	{
		ArchiveScanRecord asr = FCEUD_ScanArchive(fileToOpen);
		if (asr.numFilesInArchive < 0)
		{
			// error occurred, return
			// actually it's canceled not by user but an error message already shown
			*userCancel = 1;
			return fceufp;
		}
		asr.files.FilterByExtension(extensions);
		if(!asr.isArchive())
		{
			//if the archive contained no files, try to open it the old fashioned way
			EMUFILE_FILE* fp = FCEUD_UTF8_fstream(fileToOpen,mode);
			if(!fp)
				return 0;
			if (fp->get_fp() == NULL)
			{
				//fp is new'ed so it has to be deleted
				delete fp;
				return 0;
			}


			//try to read a zip file
			{
				fceufp = TryUnzip(fileToOpen);
				if(fceufp) {
					delete fp;
					fceufp->filename = fileToOpen;
					fceufp->logicalPath = fileToOpen;
					fceufp->fullFilename = fileToOpen;
					fceufp->archiveIndex = -1;
					goto applyips;
				}
			}

			//try to read a gzipped file
			{
				uint32 magic;

				magic = (fp->fgetc() & 0x00ff);
				magic|= (fp->fgetc() & 0x00ff) << 8;
				magic|= (fp->fgetc() & 0x00ff) << 16;
				fp->fseek(0,SEEK_SET);

				if(magic==0x088b1f) {
					 // maybe gzip...

					gzFile gzfile = gzopen(fileToOpen.c_str(),"rb");
					if(gzfile) {
						delete fp;

						size_t size;
						for(size=0; gzgetc(gzfile) != EOF; size++) {}
						EMUFILE_MEMORY* ms = new EMUFILE_MEMORY(size);
						gzseek(gzfile,0,SEEK_SET);
						gzread(gzfile,ms->buf(),size);
						gzclose(gzfile);

						fceufp = new FCEUFILE();
						fceufp->filename = fileToOpen;
						fceufp->logicalPath = fileToOpen;
						fceufp->fullFilename = fileToOpen;
						fceufp->archiveIndex = -1;
						fceufp->stream = ms;
						fceufp->size = size;
						goto applyips;
					}
				}
			}


			//open a plain old file
			fceufp = new FCEUFILE();
			fceufp->filename = fileToOpen;
			fceufp->logicalPath = fileToOpen;
			fceufp->fullFilename = fileToOpen;
			fceufp->archiveIndex = -1;
			fceufp->stream = fp;
			FCEU_fseek(fceufp,0,SEEK_END);
			fceufp->size = FCEU_ftell(fceufp);
			FCEU_fseek(fceufp,0,SEEK_SET);
			goto applyips;
		}
		else
		{
			//open an archive file
			if(archive == "")
				if(index != -1)
					fceufp = FCEUD_OpenArchiveIndex(asr, fileToOpen, index, userCancel);
				else
					fceufp = FCEUD_OpenArchive(asr, fileToOpen, 0, userCancel);
			else
				fceufp = FCEUD_OpenArchive(asr, archive, &fname, userCancel);

			if(!fceufp) return 0;

			FileBaseInfo fbi = DetermineFileBase(fileToOpen);
			fceufp->logicalPath = fbi.filebasedirectory + fceufp->filename;
			goto applyips;
		}

	applyips:
		//try to open the ips file
		if(!ipsfile && !ipsfn)
			ipsfile=FCEUD_UTF8fopen(FCEU_MakeIpsFilename(DetermineFileBase(fceufp->logicalPath.c_str())),"rb");
		ApplyIPS(ipsfile,fceufp);
		return fceufp;
	}
	return 0;
}

int FCEU_fclose(FCEUFILE *fp)
{
	delete fp;
	return 1;
}

uint64 FCEU_fread(void *ptr, size_t size, size_t nmemb, FCEUFILE *fp)
{
	return fp->stream->fread((char*)ptr,size*nmemb);
}

uint64 FCEU_fwrite(void *ptr, size_t size, size_t nmemb, FCEUFILE *fp)
{
	fp->stream->fwrite((char*)ptr,size*nmemb);
	//todo - how do we tell how many bytes we wrote?
	return nmemb;
}

int FCEU_fseek(FCEUFILE *fp, long offset, int whence)
{
	fp->stream->fseek(offset,whence);

	return FCEU_ftell(fp);
}

uint64 FCEU_ftell(FCEUFILE *fp)
{
	return fp->stream->ftell();
}

int FCEU_read16le(uint16 *val, FCEUFILE *fp)
{
	return read16le(val,fp->stream);
}

int FCEU_read32le(uint32 *Bufo, FCEUFILE *fp)
{
	return read32le(Bufo, fp->stream);
}

int FCEU_fgetc(FCEUFILE *fp)
{
	return fp->stream->fgetc();
}

uint64 FCEU_fgetsize(FCEUFILE *fp)
{
	return fp->size;
}

int FCEU_fisarchive(FCEUFILE *fp)
{
	if(fp->archiveIndex==0) return 0;
	else return 1;
}

std::string GetMfn() //Retrieves the movie filename from curMovieFilename (for adding to savestate and auto-save files)
{
	std::string movieFilenamePart;
	if (!curMovieFilename.empty())
	{
		char drv[PATH_MAX], dir[PATH_MAX], name[PATH_MAX], ext[PATH_MAX];
		splitpath(curMovieFilename.c_str(),drv,dir,name,ext);
		movieFilenamePart = std::string(".") + name;
	}
	return movieFilenamePart;
}

/// Updates the base directory
void FCEUI_SetBaseDirectory(std::string const & dir)
{
	BaseDirectory = dir;
}
/// Gets the base directory
const char *FCEUI_GetBaseDirectory(void)
{
	return BaseDirectory.c_str();
}

static char *odirs[FCEUIOD__COUNT]={0,0,0,0,0,0,0,0,0,0,0,0,0};     // odirs, odors. ^_^

void FCEUI_SetDirOverride(int which, char *n)
{
	//	FCEU_PrintError("odirs[%d]=%s->%s", which, odirs[which], n);
	if (which < FCEUIOD__COUNT)
	{
		odirs[which] = n;
	}
}

	#ifndef HAVE_ASPRINTF
	static int asprintf(char **strp, const char *fmt, ...)
	{
		va_list ap;
		int ret;

		if(!(*strp=(char*)FCEU_malloc(2048))) //mbg merge 7/17/06 cast to char*
			return(0);
		va_start(ap,fmt);
		ret=vsnprintf(*strp,2048,fmt,ap);
		va_end(ap);
		return(ret);
	}
	#endif

std::string  FCEU_GetPath(int type)
{
	switch (type)
	{
		case FCEUMKF_STATE:
		{
			if(odirs[FCEUIOD_STATES])
				return (odirs[FCEUIOD_STATES]);
			else
				return BaseDirectory + PSS + "fcs";
			break;
		}
		case FCEUMKF_CHEAT:
		{
			if(odirs[FCEUIOD_CHEATS])
				return (odirs[FCEUIOD_CHEATS]);
			else
				return BaseDirectory + PSS + "cheats";
			break;
		}
		case FCEUMKF_MOVIE:
		{
			if(odirs[FCEUIOD_MOVIES])
				return (odirs[FCEUIOD_MOVIES]);
			else
				return BaseDirectory + PSS + "movies";
			break;
		}
		case FCEUMKF_MEMW:
		{
			if(odirs[FCEUIOD_MEMW])
				return (odirs[FCEUIOD_MEMW]);
			else
				return "";	//adelikat: 03/02/09 - return null so it defaults to last directory used
				//return BaseDirectory + PSS + "tools";
			break;
		}
		//adelikat: TODO: this no longer exist and could be removed (but that would require changing a lot of other directory arrays
		case FCEUMKF_BBOT:
		{
			if(odirs[FCEUIOD_BBOT])
				return (odirs[FCEUIOD_BBOT]);
			else
				return BaseDirectory + PSS + "tools";
			break;
		}
		case FCEUMKF_ROMS:
		{
			if(odirs[FCEUIOD_ROMS])
				return (odirs[FCEUIOD_ROMS]);
			else
				return "";	//adelikat: removing base directory return, should return null it goes to last used directory
			break;
		}
		case FCEUMKF_INPUT:
		{
			if(odirs[FCEUIOD_INPUT])
				return (odirs[FCEUIOD_INPUT]);
			else
				return BaseDirectory + PSS + "tools";
			break;
		}
		case FCEUMKF_LUA:
		{
			if(odirs[FCEUIOD_LUA])
				return (odirs[FCEUIOD_LUA]);
			else
				return "";	//adelikat: 03/02/09 - return null so it defaults to last directory used //return BaseDirectory + PSS + "tools";
			break;
		}
		case FCEUMKF_AVI:
		{
			if(odirs[FCEUIOD_AVI])
				return (odirs[FCEUIOD_AVI]);
			else
				return "";		//adelikat - 03/02/09 - if no override, should return null and allow the last directory to be used intead
				//return BaseDirectory + PSS + "tools";
			break;
		}
		case FCEUMKF_TASEDITOR:
		{
			return BaseDirectory + PSS + "tools";
			break;
		}
	}
	return "";
}

std::string FCEU_MakePath(int type, const char* filebase)
{
	char ret[FILENAME_MAX];

	switch(type)
	{
		case FCEUMKF_MOVIE:
			if(odirs[FCEUIOD_MOVIES])
				return (string)odirs[FCEUIOD_MOVIES] + PSS + filebase;
			else
				return BaseDirectory + PSS + "movies" + PSS + filebase;
			break;
		case FCEUMKF_STATE:
			if(odirs[FCEUIOD_STATES])
				return (string)odirs[FCEUIOD_STATES] + PSS + filebase;
			else
				return BaseDirectory + PSS + "fcs" + PSS + filebase;
			break;
	}
	return ret;
}

std::string FCEU_MakeFName(int type, int id1, const char *cd1)
{
	char ret[FILENAME_MAX] = "";
	struct stat tmpstat;
	std::string mfnString;
	const char* mfn;	// the movie filename

	switch(type)
	{
		case FCEUMKF_MOVIE:
			struct stat fileInfo;
			do {
				if(odirs[FCEUIOD_MOVIES])
					snprintf(ret, sizeof(ret), "%s" PSS "%s-%d.fm2",odirs[FCEUIOD_MOVIES],FileBase, id1);
				else
					snprintf(ret, sizeof(ret), "%s" PSS "movies" PSS "%s-%d.fm2",BaseDirectory.c_str(),FileBase, id1);
				id1++;
			} while (stat(ret, &fileInfo) == 0);
			break;
		case FCEUMKF_STATE:
			{
				if (bindSavestate)
					mfnString = GetMfn();
				else
					mfnString = "";

				if (mfnString.length() <= MAX_MOVIEFILENAME_LEN)
				{
					mfn = mfnString.c_str();
				} else
				{
					//This caps the movie filename length before adding it to the savestate filename.
					//This helps prevent possible crashes from savestate filenames of excessive length.
					mfnString = mfnString.substr(0, MAX_MOVIEFILENAME_LEN);
					mfn = mfnString.c_str();
				}

				if(odirs[FCEUIOD_STATES])
				{
					snprintf(ret, sizeof(ret), "%s" PSS "%s%s.fc%d",odirs[FCEUIOD_STATES],FileBase,mfn,id1);
				} else
				{
					snprintf(ret, sizeof(ret), "%s" PSS "fcs" PSS "%s%s.fc%d",BaseDirectory.c_str(),FileBase,mfn,id1);
				}
				if(stat(ret,&tmpstat)==-1)
				{
					if(odirs[FCEUIOD_STATES])
					{
						snprintf(ret, sizeof(ret), "%s" PSS "%s%s.fc%d",odirs[FCEUIOD_STATES],FileBase,mfn,id1);
					} else
					{
						snprintf(ret, sizeof(ret), "%s" PSS "fcs" PSS "%s%s.fc%d",BaseDirectory.c_str(),FileBase,mfn,id1);
					}
				}
			}
			break;
		case FCEUMKF_RESUMESTATE:
			{
				if(odirs[FCEUIOD_STATES])
				{
					snprintf(ret, sizeof(ret), "%s" PSS "%s-resume.fcs",odirs[FCEUIOD_STATES],FileBase);
				} else
				{
					snprintf(ret, sizeof(ret), "%s" PSS "fcs" PSS "%s-resume.fcs",BaseDirectory.c_str(),FileBase);
				}
				if(stat(ret,&tmpstat)==-1)
				{
					if(odirs[FCEUIOD_STATES])
					{
						snprintf(ret, sizeof(ret), "%s" PSS "%s-resume.fcs",odirs[FCEUIOD_STATES],FileBase);
					} else
					{
						snprintf(ret, sizeof(ret), "%s" PSS "fcs" PSS "%s-resume.fcs",BaseDirectory.c_str(),FileBase);
					}
				}
			}
			break;
		case FCEUMKF_SNAP:
			if(odirs[FCEUIOD_SNAPS])
				snprintf(ret, sizeof(ret), "%s" PSS "%s-%d.%s",odirs[FCEUIOD_SNAPS],FileBase,id1,cd1);
			else
				snprintf(ret, sizeof(ret), "%s" PSS "snaps" PSS "%s-%d.%s",BaseDirectory.c_str(),FileBase,id1,cd1);
			break;
		case FCEUMKF_FDS:
			if(odirs[FCEUIOD_NV])
				snprintf(ret, sizeof(ret), "%s" PSS "%s.fds",odirs[FCEUIOD_NV],FileBase);
			else
				snprintf(ret, sizeof(ret), "%s" PSS "sav" PSS "%s.fds",BaseDirectory.c_str(),FileBase);
			break;
		case FCEUMKF_SAV:
			if(odirs[FCEUIOD_NV])
				snprintf(ret, sizeof(ret), "%s" PSS "%s.%s",odirs[FCEUIOD_NV],FileBase,cd1);
			else
				snprintf(ret, sizeof(ret), "%s" PSS "sav" PSS "%s.%s",BaseDirectory.c_str(),FileBase,cd1);
			if(stat(ret,&tmpstat)==-1)
			{
				if(odirs[FCEUIOD_NV])
					snprintf(ret, sizeof(ret), "%s" PSS "%s.%s",odirs[FCEUIOD_NV],FileBase,cd1);
				else
					snprintf(ret, sizeof(ret), "%s" PSS "sav" PSS "%s.%s",BaseDirectory.c_str(),FileBase,cd1);
			}
			break;
		case FCEUMKF_AUTOSTATE:
			mfnString = GetMfn();
			if (mfnString.length() <= MAX_MOVIEFILENAME_LEN)
			{
				mfn = mfnString.c_str();
			} else
			{
				//This caps the movie filename length before adding it to the savestate filename.
				//This helps prevent possible crashes from savestate filenames of excessive length.
				mfnString = mfnString.substr(0, MAX_MOVIEFILENAME_LEN);
				mfn = mfnString.c_str();
			}

			if(odirs[FCEUIOD_STATES])
			{
				snprintf(ret, sizeof(ret), "%s" PSS "%s%s-autosave%d.fcs",odirs[FCEUIOD_STATES],FileBase,mfn,id1);
			} else
			{
				snprintf(ret, sizeof(ret), "%s" PSS "fcs" PSS "%s%s-autosave%d.fcs",BaseDirectory.c_str(),FileBase,mfn,id1);
			}
			if(stat(ret,&tmpstat)==-1)
			{
				if(odirs[FCEUIOD_STATES])
				{
					snprintf(ret, sizeof(ret), "%s" PSS "%s%s-autosave%d.fcs",odirs[FCEUIOD_STATES],FileBase,mfn,id1);
				} else
				{
					snprintf(ret, sizeof(ret), "%s" PSS "fcs" PSS "%s%s-autosave%d.fcs",BaseDirectory.c_str(),FileBase,mfn,id1);
				}
			}
			break;
		case FCEUMKF_CHEAT:
			if(odirs[FCEUIOD_CHEATS])
				snprintf(ret, sizeof(ret), "%s" PSS "%s.cht",odirs[FCEUIOD_CHEATS],FileBase);
			else
				snprintf(ret, sizeof(ret), "%s" PSS "cheats" PSS "%s.cht",BaseDirectory.c_str(),FileBase);
			break;
		case FCEUMKF_IPS:
			strcpy(ret,FCEU_MakeIpsFilename(CurrentFileBase()).c_str());
			break;
		case FCEUMKF_GGROM:snprintf(ret, sizeof(ret), "%s" PSS "gg.rom",BaseDirectory.c_str());break;
		case FCEUMKF_FDSROM:
			if(odirs[FCEUIOD_FDSROM])
				snprintf(ret, sizeof(ret), "%s" PSS "disksys.rom",odirs[FCEUIOD_FDSROM]);
			else
				snprintf(ret, sizeof(ret), "%s" PSS "disksys.rom",BaseDirectory.c_str());
			break;
		case FCEUMKF_PALETTE:snprintf(ret, sizeof(ret), "%s" PSS "%s.pal",BaseDirectory.c_str(),FileBase);break;
		case FCEUMKF_MOVIEGLOB:
			//these globs use ??? because we can load multiple formats
			if(odirs[FCEUIOD_MOVIES])
				snprintf(ret, sizeof(ret), "%s" PSS "*.???",odirs[FCEUIOD_MOVIES]);
			else
				snprintf(ret, sizeof(ret), "%s" PSS "movies" PSS "*.???",BaseDirectory.c_str());
			break;
		case FCEUMKF_MOVIEGLOB2:snprintf(ret, sizeof(ret), "%s" PSS "*.???",BaseDirectory.c_str());break;
		case FCEUMKF_STATEGLOB:
			if(odirs[FCEUIOD_STATES])
				snprintf(ret, sizeof(ret), "%s" PSS "%s*.fc?",odirs[FCEUIOD_STATES],FileBase);
			else
				snprintf(ret, sizeof(ret), "%s" PSS "fcs" PSS "%s*.fc?",BaseDirectory.c_str(),FileBase);
			break;
	}

	//convert | to . for archive filenames.
	return mass_replace(ret,"|",".");
}

void GetFileBase(const char *f)
{
	FileBaseInfo fbi = DetermineFileBase(f);
	strcpy(FileBase,fbi.filebase.c_str());
	strcpy(FileBaseDirectory,fbi.filebasedirectory.c_str());
}

bool FCEU_isFileInArchive(const char *path)
{
	bool isarchive = false;
	FCEUFILE* fp = FCEU_fopen(path,0,"rb",0,0);
	if(fp) {
		isarchive = fp->isArchive();
		delete fp;
	}
	return isarchive;
}



void FCEUARCHIVEFILEINFO::FilterByExtension(const char** ext)
{
	if(!ext) return;
	int count = size();
	for(int i=count-1;i>=0;i--) {
		std::string fext = getExtension((*this)[i].name.c_str());
		const char** currext = ext;
		while(*currext) {
			if(fext == *currext)
				goto ok;
			currext++;
		}
		this->erase(begin()+i);
	ok: ;
	}
}
