#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <3ds.h>

#include "filesystem.h"
#include "blz.h"
#include "savegame_data.h"

char status[256];

char regionids_table[7][4] = {//http://3dbrew.org/wiki/Nandrw/sys/SecureInfo_A
"JPN",
"USA",
"EUR",
"JPN", //"AUS"
"CHN",
"KOR",
"TWN"
};

Result FSUSER_ControlArchive(Handle handle, FS_archive archive)
{
	u32* cmdbuf=getThreadCommandBuffer();

	u32 b1 = 0, b2 = 0;

	cmdbuf[0]=0x080d0144;
	cmdbuf[1]=archive.handleLow;
	cmdbuf[2]=archive.handleHigh;
	cmdbuf[3]=0x0;
	cmdbuf[4]=0x1; //buffer1 size
	cmdbuf[5]=0x1; //buffer1 size
	cmdbuf[6]=0x1a;
	cmdbuf[7]=(u32)&b1;
	cmdbuf[8]=0x1c;
	cmdbuf[9]=(u32)&b2;
 
	Result ret=0;
	if((ret=svcSendSyncRequest(handle)))return ret;
 
	return cmdbuf[1];
}

Result write_savedata(char* path, u8* data, u32 size)
{
	if(!path || !data || !size)return -1;

	Handle outFileHandle;
	u32 bytesWritten;
	Result ret = 0;
	int fail = 0;

	ret = FSUSER_OpenFile(&saveGameFsHandle, &outFileHandle, saveGameArchive, FS_makePath(PATH_CHAR, path), FS_OPEN_CREATE | FS_OPEN_WRITE, FS_ATTRIBUTE_NONE);
	if(ret){fail = -8; goto writeFail;}

	ret = FSFILE_Write(outFileHandle, &bytesWritten, 0x0, data, size, 0x10001);
	if(ret){fail = -9; goto writeFail;}

	ret = FSFILE_Close(outFileHandle);
	if(ret){fail = -10; goto writeFail;}

	ret = FSUSER_ControlArchive(saveGameFsHandle, saveGameArchive);

	writeFail:
	if(fail)sprintf(status, "failed to write to file : %d\n     %08X %08X", fail, (unsigned int)ret, (unsigned int)bytesWritten);
	else sprintf(status, "successfully wrote to file !\n     %08X               ", (unsigned int)bytesWritten);

	return ret;
}

typedef enum
{
	STATE_NONE,
	STATE_INITIAL,
	STATE_SELECT_SLOT,
	STATE_SELECT_IRON_VERSION,
	STATE_SELECT_FIRMWARE,
	STATE_DOWNLOAD_PAYLOAD,
	STATE_COMPRESS_PAYLOAD,
	STATE_INSTALL_PAYLOAD,
	STATE_INSTALLED_PAYLOAD,
	STATE_ERROR,
}state_t;

Result http_getredirection(char *url, char *out, u32 out_size)
{
	Result ret=0;
	httpcContext context;

	ret = httpcOpenContext(&context, url, 0);
	if(ret!=0)return ret;


	ret = httpcAddRequestHeaderField(&context, "User-Agent", "ironhax");
	if(!ret) ret = httpcBeginRequest(&context);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcGetResponseHeader(&context, "Location", out, out_size);

	httpcCloseContext(&context);

	return 0;
}

Result http_download(httpcContext *context, u8** out_buf, u32* out_size)
{
	Result ret=0;
	u32 statuscode=0;
	u32 contentsize=0;
	u8 *buf;

	ret = httpcBeginRequest(context);
	if(ret!=0)return ret;

	ret = httpcGetResponseStatusCode(context, &statuscode, 0);
	if(ret!=0)return ret;

	if(statuscode!=200)return -2;

	ret=httpcGetDownloadSizeState(context, NULL, &contentsize);
	if(ret!=0)return ret;

	buf = (u8*)malloc(contentsize);
	if(buf==NULL)return -1;
	memset(buf, 0, contentsize);

	ret = httpcDownloadData(context, buf, contentsize, NULL);
	if(ret!=0)
	{
		free(buf);
		return ret;
	}

	if(out_size)*out_size = contentsize;
	if(out_buf)*out_buf = buf;
	else free(buf);

	return 0;
}


int main()
{
	httpcInit();

	gfxInitDefault();
	gfxSet3D(false);

	filesystemInit();

	PrintConsole topConsole, bttmConsole;
	consoleInit(GFX_TOP, &topConsole);
	consoleInit(GFX_BOTTOM, &bttmConsole);

	consoleSelect(&topConsole);
	consoleClear();

	state_t current_state = STATE_NONE;
	state_t next_state = STATE_INITIAL;

	static char top_text[2048];
	top_text[0] = '\0';

	int selected_slot = 0;
	int selected_iron_version = 0;

	int firmware_version[6];
	int firmware_selected_value = 0;
	int firmware_version_autodetected = 0;
	int firmware_maxnum;

	int pos;
	
	u8* payload_buf = NULL;
	u32 payload_size = 0;

	u32 cur_processid = 0;
	u64 cur_programid = 0;
	u64 cur_programid_update = 0;
	u8 update_mediatype = 1;
	FS_ProductInfo cur_productinfo;
	TitleList title_entry;

	OS_VersionBin nver_versionbin, cver_versionbin;
	u8 region=0;
	u8 new3dsflag = 0;

	memset(firmware_version, 0, sizeof(firmware_version));

	while (aptMainLoop())
	{
		hidScanInput();
		if(hidKeysDown() & KEY_START)break;

		// transition function
		if(next_state != current_state)
		{
			switch(next_state)
			{
				case STATE_INITIAL:
					strcat(top_text, " Welcome to the ironhax installer ! Please proceedwith caution, as you might lose data if you don't.You may press START at any time to return to menu.\n                            Press A to continue.\n\n");
					break;
				case STATE_SELECT_SLOT:
					strcat(top_text, " Please select the savegame slot IRONHAX will be\ninstalled to. D-Pad to select, A to continue.\n");
					break;
				case STATE_SELECT_IRON_VERSION:
					strcat(top_text, "\n\n\n The version of IRONFALL you have installed\nwill now be auto-detected.\n");
					break;
				case STATE_SELECT_FIRMWARE:
					strcat(top_text, "\n\n\n Please select your console's firmware version.\nOnly select NEW 3DS if you own a New 3DS (XL).\nD-Pad to select, A to continue.\n");
					break;
				case STATE_DOWNLOAD_PAYLOAD:
					sprintf(top_text, "%s\n\n\n Downloading payload...\n", top_text);
					break;
				case STATE_COMPRESS_PAYLOAD:
					strcat(top_text, " Processing payload...\n");
					break;
				case STATE_INSTALL_PAYLOAD:
					strcat(top_text, " Installing payload...\n");
					break;
				case STATE_INSTALLED_PAYLOAD:
					strcat(top_text, " Done ! ironhax was successfully installed.");
					break;
				case STATE_ERROR:
					strcat(top_text, " Looks like something went wrong. :(\n");
					break;
				default:
					break;
			}
			current_state = next_state;
		}

		consoleSelect(&topConsole);
		printf("\x1b[0;%dHironhax installer", (50 - 17) / 2);
		printf("\x1b[1;%dHby smea\n\n\n", (50 - 7) / 2);
		printf(top_text);

		// state function
		switch(current_state)
		{
			case STATE_INITIAL:
				if(hidKeysDown() & KEY_A)next_state = STATE_SELECT_SLOT;
				break;
			case STATE_SELECT_SLOT:
				{
					if(hidKeysDown() & KEY_UP)selected_slot++;
					if(hidKeysDown() & KEY_DOWN)selected_slot--;
					if(hidKeysDown() & KEY_A)next_state = STATE_SELECT_IRON_VERSION;

					if(selected_slot < 0) selected_slot = 0;
					if(selected_slot > 2) selected_slot = 2;

					printf((selected_slot >= 2) ? "                                             \n" : "                                            ^\n");
					printf("                            Selected slot : %d  \n", selected_slot + 1);
					printf((!selected_slot) ? "                                             \n" : "                                            v\n");
				}
				break;
			case STATE_SELECT_IRON_VERSION:
				{
					if(hidKeysDown() & KEY_A)next_state = STATE_SELECT_FIRMWARE;

					Result ret = svcGetProcessId(&cur_processid, 0xffff8001);
					if(ret<0)
					{
						snprintf(status, sizeof(status)-1, "Failed to get the processID for the current process.\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					ret = FSUSER_GetProductInfo(NULL, cur_processid, &cur_productinfo);
					if(ret<0)
					{
						snprintf(status, sizeof(status)-1, "Failed to get the ProductInfo for the current process.\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					aptOpenSession();
					ret = APT_GetProgramID(NULL, &cur_programid);
					aptCloseSession();

					if(ret<0)
					{
						snprintf(status, sizeof(status)-1, "Failed to get the programID for the current process.\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					if(((cur_programid >> 32) & 0xffff) == 0)cur_programid_update = cur_programid | 0x0000000e00000000ULL;//Only set the update-title programid when the cur_programid is for a regular application title.

					if(cur_productinfo.remaster_version>=2)
					{
						snprintf(status, sizeof(status)-1, "this regular-title remaster-version(%u) of ironfall is not compatible with ironhax, sorry\n", cur_productinfo.remaster_version);
						next_state = STATE_ERROR;
						break;
					}

					if(cur_programid_update)
					{
						ret = amInit();
						if(ret<0)
						{
							snprintf(status, sizeof(status)-1, "Failed to initialize AM.\n    Error code : %08X", (unsigned int)ret);
							next_state = STATE_ERROR;
							break;
						}

						ret = AM_ListTitles(update_mediatype, 1, &cur_programid_update, &title_entry);
						amExit();
						if(ret==0)
						{
							if(title_entry.titleVersion != 1040)
							{
								snprintf(status, sizeof(status)-1, "this update-title version(%u) of ironfall is not compatible with ironhax, sorry\n", title_entry.titleVersion);
								next_state = STATE_ERROR;
								break;
							}

							selected_iron_version = 1;
						}
					}

					printf("           Auto-detected IRONFALL version : 1.%d  \n    Press A to continue.", selected_iron_version);
				}
				break;
			case STATE_SELECT_FIRMWARE:
				{
					if(hidKeysDown() & KEY_LEFT)firmware_selected_value--;
					if(hidKeysDown() & KEY_RIGHT)firmware_selected_value++;

					if(firmware_selected_value < 0) firmware_selected_value = 0;
					if(firmware_selected_value > 5) firmware_selected_value = 5;

					if(hidKeysDown() & KEY_UP)firmware_version[firmware_selected_value]++;
					if(hidKeysDown() & KEY_DOWN)firmware_version[firmware_selected_value]--;

					firmware_maxnum = 256;
					if(firmware_selected_value==0)firmware_maxnum = 2;
					if(firmware_selected_value==5)firmware_maxnum = 7;

					if(firmware_version[firmware_selected_value] < 0) firmware_version[firmware_selected_value] = 0;
					if(firmware_version[firmware_selected_value] >= firmware_maxnum) firmware_version[firmware_selected_value] = firmware_maxnum - 1;

					if(hidKeysDown() & KEY_A)next_state = STATE_DOWNLOAD_PAYLOAD;

					if(firmware_version_autodetected==0)
					{
						Result ret = osGetSystemVersionData(&nver_versionbin, &cver_versionbin);
						if(ret<0)
						{
							snprintf(status, sizeof(status)-1, "Failed to get the system-version.\n    Error code : %08X", (unsigned int)ret);
							next_state = STATE_ERROR;
							break;
						}

						ret = initCfgu();
						if(ret<0)
						{
							snprintf(status, sizeof(status)-1, "Failed to initialize cfgu.\n    Error code : %08X", (unsigned int)ret);
							next_state = STATE_ERROR;
							break;
						}

						ret = CFGU_SecureInfoGetRegion(&region);
						if(ret<0)
						{
							snprintf(status, sizeof(status)-1, "Failed to get the system region.\n    Error code : %08X", (unsigned int)ret);
							next_state = STATE_ERROR;
							break;
						}

						exitCfgu();

						APT_CheckNew3DS(NULL, &new3dsflag);

						firmware_version[0] = new3dsflag;
						firmware_version[5] = region;

						firmware_version[1] = cver_versionbin.mainver;
						firmware_version[2] = cver_versionbin.minor;
						firmware_version[3] = cver_versionbin.build;
						firmware_version[4] = nver_versionbin.mainver;

						firmware_version_autodetected = 1;
					}

					int offset = 26;
					if(firmware_selected_value)
					{
						offset+= 7;

						for(pos=1; pos<firmware_selected_value; pos++)
						{
							offset+=2;
							if(firmware_version[pos] >= 10)offset++;
						}
					}

					printf((firmware_version[firmware_selected_value] < firmware_maxnum - 1) ? "%*s^%*s" : "%*s-%*s", offset, " ", 50 - offset - 1, " ");
					printf("      Selected firmware : %s %d-%d-%d-%d %s  \n", firmware_version[0]?"New3DS":"Old3DS", firmware_version[1], firmware_version[2], firmware_version[3], firmware_version[4], regionids_table[firmware_version[5]]);
					printf((firmware_version[firmware_selected_value] > 0) ? "%*sv%*s" : "%*s-%*s", offset, " ", 50 - offset - 1, " ");
				}
				break;
			case STATE_DOWNLOAD_PAYLOAD:
				{
					httpcContext context;
					static char in_url[512];
					static char out_url[512];

					if(firmware_version[5]!=1 && firmware_version[5]!=2)
					{
						snprintf(status, sizeof(status)-1, "The specified region is not supported by ironhax.\n");
						next_state = STATE_ERROR;
						break;
					}

					sprintf(in_url, "http://smea.mtheall.com/get_payload.php?version=%s-%d-%d-%d-%d-%s", firmware_version[0]?"NEW":"OLD", firmware_version[1], firmware_version[2], firmware_version[3], firmware_version[4], regionids_table[firmware_version[5]]);

					Result ret = http_getredirection(in_url, out_url, 512);
					if(ret)
					{
						sprintf(status, "Failed to grab payload url\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					ret = httpcOpenContext(&context, out_url, 0);
					if(ret)
					{
						sprintf(status, "Failed to open http context\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					ret = http_download(&context, &payload_buf, &payload_size);
					if(ret)
					{
						sprintf(status, "Failed to download payload\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					next_state = STATE_COMPRESS_PAYLOAD;
				}
				break;
			case STATE_COMPRESS_PAYLOAD:
				payload_buf = BLZ_Code(payload_buf, payload_size, (unsigned int*)&payload_size, BLZ_NORMAL);
				next_state = STATE_INSTALL_PAYLOAD;
				break;
			case STATE_INSTALL_PAYLOAD:
				{
					static char filename[128];
					sprintf(filename, "/Data%d", selected_slot);
					Result ret = write_savedata(filename, getSavegameData(firmware_version[5], firmware_version[0], selected_iron_version, selected_slot), 0x2000);
					if(ret)
					{
						sprintf(status, "Failed to install %s.\n    Error code : %08X", filename, (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}
				}

				{
					// delete file
					FSUSER_DeleteFile(&saveGameFsHandle, saveGameArchive, FS_makePath(PATH_CHAR, "/payload.bin"));

					FSUSER_ControlArchive(saveGameFsHandle, saveGameArchive);
				}

				{
					Result ret = write_savedata("/payload.bin", payload_buf, payload_size);
					if(ret)
					{
						sprintf(status, "Failed to install payload\n    Error code : %08X", (unsigned int)ret);
						next_state = STATE_ERROR;
						break;
					}

					next_state = STATE_INSTALLED_PAYLOAD;	
				}
				break;
			case STATE_INSTALLED_PAYLOAD:
				next_state = STATE_NONE;
				break;
			default:
				break;
		}

		consoleSelect(&bttmConsole);
		printf("\x1b[0;0H  Current status :\n    %s\n", status);

		gspWaitForVBlank();
	}

	filesystemExit();

	gfxExit();
	httpcExit();
	return 0;
}
