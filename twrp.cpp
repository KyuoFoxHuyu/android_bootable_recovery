/*
	This file is part of TWRP/TeamWin Recovery Project.

	Copyright (C) 2018-2019 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include "gui/twmsg.h"

#include "cutils/properties.h"
#include "bootloader_message_twrp/include/bootloader_message_twrp/bootloader_message.h"

#ifdef ANDROID_RB_RESTART
#include "cutils/android_reboot.h"
#else
#include <sys/reboot.h>
#endif

extern "C"
{
#include "gui/gui.h"
}
#include "set_metadata.h"
#include "gui/gui.hpp"
#include "gui/pages.hpp"
#include "gui/objects.hpp"
#include "twcommon.h"
#include "twrp-functions.hpp"
#include "data.hpp"
#include "partitions.hpp"
#include "openrecoveryscript.hpp"
#include "variables.h"
#include "twrpAdbBuFifo.hpp"
#ifdef TW_USE_NEW_MINADBD
#include "minadbd/minadbd.h"
#else
extern "C"
{
#include "minadbd21/adb.h"
}
#endif

//extern int adb_server_main(int is_daemon, int server_port, int /* reply_fd */);

TWPartitionManager PartitionManager;
int Log_Offset;
bool datamedia;

static void Print_Prop(const char *key, const char *name, void *cookie)
{
  printf("%s=%s\n", key, name);
}

int main(int argc, char **argv)
{
   //[f/d] Disable LED as early as possible
   DataManager::Leds(false);

  // Recovery needs to install world-readable files, so clear umask
  // set by init
  umask(0);

  Log_Offset = 0;

  // Set up temporary log file (/tmp/recovery.log)
  freopen(TMP_LOG_FILE, "a", stdout);
  setbuf(stdout, NULL);
  freopen(TMP_LOG_FILE, "a", stderr);
  setbuf(stderr, NULL);

  signal(SIGPIPE, SIG_IGN);

  // Handle ADB sideload
  if (argc == 3 && strcmp(argv[1], "--adbd") == 0)
    {
      property_set("ctl.stop", "adbd");
#ifdef TW_USE_NEW_MINADBD
      //adb_server_main(0, DEFAULT_ADB_PORT, -1); TODO fix this for android8
      minadbd_main();
#else
      adb_main(argv[2]);
#endif
      return 0;
    }

#ifdef RECOVERY_SDCARD_ON_DATA
  datamedia = true;
#endif

  char crash_prop_val[PROPERTY_VALUE_MAX];
  int crash_counter;
  property_get("twrp.crash_counter", crash_prop_val, "-1");
  crash_counter = atoi(crash_prop_val) + 1;
  snprintf(crash_prop_val, sizeof(crash_prop_val), "%d", crash_counter);
  property_set("twrp.crash_counter", crash_prop_val);
  property_set("ro.twrp.boot", "1");
  property_set("ro.twrp.build", "orangefox");
  property_set("ro.twrp.version", FOX_VERSION);
  
  string fox_build_date = TWFunc::File_Property_Get ("/etc/fox.cfg", "FOX_BUILD_DATE");
  if (fox_build_date == "")
     {
        fox_build_date = TWFunc::File_Property_Get ("/default.prop", "ro.bootimage.build.date");
        if (fox_build_date == "")
          {
              fox_build_date = TWFunc::File_Property_Get ("/default.prop", "ro.build.date");
              if (fox_build_date == "")
                 fox_build_date = "[no date!]";
         }
     }

  DataManager::GetValue(FOX_COMPATIBILITY_DEVICE, Fox_Current_Device);
  time_t StartupTime = time(NULL);
  printf("Starting OrangeFox TWRP %s-%s-%s (built on %s for %s) on %s (pid %d)\n", FOX_VERSION,
	 FOX_BUILD, TW_GIT_REVISION, fox_build_date.c_str(), Fox_Current_Device.c_str(), ctime(&StartupTime), getpid());

  // Load default values to set DataManager constants and handle ifdefs
	DataManager::SetDefaultValues();
	printf("Starting the UI...\n");
	gui_init();
	printf("=> Linking mtab\n");
	symlink("/proc/mounts", "/etc/mtab");
	std::string fstab_filename = "/etc/twrp.fstab";
	if (!TWFunc::Path_Exists(fstab_filename)) {
		fstab_filename = "/etc/recovery.fstab";
	}
	printf("=> Processing %s\n", fstab_filename.c_str());
	if (!PartitionManager.Process_Fstab(fstab_filename, 1)) {
		LOGERR("Failing out of recovery due to problem with fstab.\n");
		return -1;
	}
	PartitionManager.Output_Partition_Logging();
	// Load up all the resources
	gui_loadResources();

	bool Shutdown = false;
	bool SkipDecryption = false;
	int Need_Decrypt = 0;
	
	string Send_Intent = "";
	{
		TWPartition* misc = PartitionManager.Find_Partition_By_Path("/misc");
		if (misc != NULL) {
			if (misc->Current_File_System == "emmc") {
				set_misc_device(misc->Actual_Block_Device.c_str());
			} else {
				LOGERR("Only emmc /misc is supported\n");
			}
		}
		get_args(&argc, &argv);

		int index, index2, len;
		char* argptr;
		char* ptr;
		printf("Startup Commands: ");
		for (index = 1; index < argc; index++) {
			if (strcmp(argv[index], "--prompt_and_wipe_data") == 0) // Rescue Party ?
			   {
			      gui_err("OrangeFox: ROM Rescue Party trigger! You need to format your data partition!\n");
			   }

			argptr = argv[index];
			printf(" '%s'", argv[index]);
			len = strlen(argv[index]);
			if (*argptr == '-') {argptr++; len--;}
			if (*argptr == '-') {argptr++; len--;}
			if (*argptr == 'u') {
				ptr = argptr;
				index2 = 0;
				while (*ptr != '=' && *ptr != '\n')
					ptr++;
				// skip the = before grabbing Zip_File
				while (*ptr == '=')
					ptr++;
				if (*ptr) {
					string ORSCommand = "install ";
					ORSCommand.append(ptr);

					// If we have a map of blocks we don't need to mount data.
					SkipDecryption = *ptr == '@';
					if (*ptr == '@')
					   Need_Decrypt = 1;
					else Need_Decrypt = 0;

					if (!OpenRecoveryScript::Insert_ORS_Command(ORSCommand))
						break;
				} else
					LOGERR("argument error specifying zip file\n");
			} else if (*argptr == 'w') {
				if (len == 9) {
					if (!OpenRecoveryScript::Insert_ORS_Command("wipe data\n"))
						break;
				} else if (len == 10) {
					if (!OpenRecoveryScript::Insert_ORS_Command("wipe cache\n"))
						break;
				}
				// Other 'w' items are wipe_ab and wipe_package_size which are related to bricking the device remotely. We will not bother to suppor these as having TWRP probably makes "bricking" the device in this manner useless
			} else if (*argptr == 'n') {
				DataManager::SetValue(TW_BACKUP_NAME, gui_parse_text("{@auto_generate}"));
				if (!OpenRecoveryScript::Insert_ORS_Command("backup BSDCAE\n"))
					break;
			} else if (*argptr == 'p') {
				Shutdown = true;
			} else if (*argptr == 's') {
				if (strncmp(argptr, "send_intent", strlen("send_intent")) == 0) {
					ptr = argptr + strlen("send_intent") + 1;
					Send_Intent = *ptr;
				} else if (strncmp(argptr, "security", strlen("security")) == 0) {
					LOGINFO("Security update\n");
				} else if (strncmp(argptr, "sideload", strlen("sideload")) == 0) {
					if (!OpenRecoveryScript::Insert_ORS_Command("sideload\n"))
						break;
				} else if (strncmp(argptr, "stages", strlen("stages")) == 0) {
					LOGINFO("ignoring stages command\n");
				}
			} else if (*argptr == 'r') {
				if (strncmp(argptr, "reason", strlen("reason")) == 0) {
					ptr = argptr + strlen("reason") + 1;
					gui_print("%s\n", ptr);
				}
			}
		}
		printf("\n");
	}

	if (crash_counter == 0) {
		property_list(Print_Prop, NULL);
		printf("\n");
	} else {
		printf("twrp.crash_counter=%d\n", crash_counter);
	}

	// Check for and run startup script if script exists
	// TWFunc::check_and_run_script("/sbin/runatboot.sh", "boot");
	// TWFunc::check_and_run_script("/sbin/postrecoveryboot.sh", "boot");

#ifdef TW_INCLUDE_INJECTTWRP
	// Back up TWRP Ramdisk if needed:
	TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
	LOGINFO("Backing up TWRP ramdisk...\n");
	if (Boot == NULL || Boot->Current_File_System != "emmc")
		TWFunc::Exec_Cmd("injecttwrp --backup /tmp/backup_recovery_ramdisk.img");
	else {
		string injectcmd = "injecttwrp --backup /tmp/backup_recovery_ramdisk.img bd=" + Boot->Actual_Block_Device;
		TWFunc::Exec_Cmd(injectcmd);
	}
	LOGINFO("Backup of TWRP ramdisk done.\n");
#endif

	// Offer to decrypt if the device is encrypted
	if (DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0) {
		if (SkipDecryption) {
		   #ifdef OF_OTA_RES_DECRYPT
			if (Need_Decrypt == 1)
			 {
			    usleep(16);
			    if (gui_startPage("decrypt", 1, 1) == 0)
			       {  
				  LOGINFO("- DEBUG: OrangeFox OTA: detected custom encryption\n");
				  DataManager::SetValue("OTA_decrypted", "1");
				  TWFunc::check_selinux_support();
				  gui_loadCustomResources();
			       } 
			 }
			else //
		   #endif
			  LOGINFO("Skipping decryption\n");			
		} else {
			LOGINFO("Is encrypted, do decrypt page first\n");			
			//
			LOGINFO("- DEBUG: OrangeFox: detected custom encryption\n");
			DataManager::SetValue("used_custom_encryption", "1");
			usleep(16);
			//
			if (gui_startPage("decrypt", 1, 1) != 0) {
				LOGERR("Failed to start decrypt GUI page.\n");
			} else {
				// Check for and load custom theme if present
				TWFunc::check_selinux_support();
				gui_loadCustomResources();
			}
		}
	} else if (datamedia) {
		TWFunc::check_selinux_support();
		if (tw_get_default_metadata(DataManager::GetSettingsStoragePath().c_str()) != 0) {
			LOGINFO("Failed to get default contexts and file mode for storage files.\n");
		} else {
			LOGINFO("Got default contexts and file mode for storage files.\n");
		}
	}

	// Fixup the RTC clock on devices which require it
	if (crash_counter == 0)
		TWFunc::Fixup_Time_On_Boot();

	// Read the settings file
	TWFunc::Update_Log_File();
	DataManager::ReadSettingsFile();
	PageManager::LoadLanguage(DataManager::GetStrValue("tw_language"));
	GUIConsole::Translate_Now();

	// Run any outstanding OpenRecoveryScript
	std::string cacheDir = TWFunc::get_cache_dir();
	std::string orsFile = cacheDir + "/recovery/openrecoveryscript";
	if ((DataManager::GetIntValue(TW_IS_ENCRYPTED) == 0 || SkipDecryption) && (TWFunc::Path_Exists(SCRIPT_FILE_TMP) || TWFunc::Path_Exists(orsFile))) {
		OpenRecoveryScript::Run_OpenRecoveryScript();
	}

#ifdef TW_HAS_MTP
  char mtp_crash_check[PROPERTY_VALUE_MAX];
  property_get("mtp.crash_check", mtp_crash_check, "0");
  if (DataManager::GetIntValue("tw_mtp_enabled")
      && !strcmp(mtp_crash_check, "0") && !crash_counter
      && (!DataManager::GetIntValue(TW_IS_ENCRYPTED)
	  || DataManager::GetIntValue(TW_IS_DECRYPTED)))
    {
      /*property_set("mtp.crash_check", "1");
      LOGINFO("Starting MTP\n");
      if (!PartitionManager.Enable_MTP())
	PartitionManager.Disable_MTP();
      else
	gui_msg("mtp_enabled=MTP Enabled");
      property_set("mtp.crash_check", "0");*/
    }
  else if (strcmp(mtp_crash_check, "0"))
    {
      gui_warn("mtp_crash=MTP Crashed, not starting MTP on boot.");
      DataManager::SetValue("tw_mtp_enabled", 0);
      PartitionManager.Disable_MTP();
    }
  else if (crash_counter == 1)
    {
      LOGINFO("TWRP crashed; disabling MTP as a precaution.\n");
      PartitionManager.Disable_MTP();
    }
#endif

  // call OrangeFox startup code
  TWFunc::OrangeFox_Startup();

#ifndef TW_OEM_BUILD
	// Check if system has never been changed
	TWPartition* sys = PartitionManager.Find_Partition_By_Path(PartitionManager.Get_Android_Root_Path());
	TWPartition* ven = PartitionManager.Find_Partition_By_Path("/vendor");

  if (sys)
    {
      if ((DataManager::GetIntValue("tw_mount_system_ro") == 0
	   && sys->Check_Lifetime_Writes() == 0)
	  || DataManager::GetIntValue("tw_mount_system_ro") == 2)
	{
	  if (DataManager::GetIntValue("tw_never_show_system_ro_page") == 0)
	    {
	      DataManager::SetValue("tw_back", "main");
	      if (gui_startPage("system_readonly", 1, 1) != 0)
		{
		  LOGERR("Failed to start system_readonly GUI page.\n");
		}
	    }
	  else if (DataManager::GetIntValue("tw_mount_system_ro") == 0)
	    {
	      sys->Change_Mount_Read_Only(false);
	      if (ven)
		ven->Change_Mount_Read_Only(false);
	    }
	}
      else if (DataManager::GetIntValue("tw_mount_system_ro") == 1)
	{
	  // Do nothing, user selected to leave system read only
	}
      else
	{
	  sys->Change_Mount_Read_Only(false);
	  if (ven)
	    ven->Change_Mount_Read_Only(false);
	}
    }
#endif
  twrpAdbBuFifo *adb_bu_fifo = new twrpAdbBuFifo();
  adb_bu_fifo->threadAdbBuFifo();

// retain dm-verity and/or forced-encryption on devices that have problems with disabling them
#ifdef OF_KEEP_FORCED_ENCRYPTION
  DataManager::SetValue(FOX_DISABLE_FORCED_ENCRYPTION, "0");
  DataManager::SetValue(FOX_ADVANCED_STOCK_REPLACE, "1");
#endif

#ifdef OF_KEEP_DM_VERITY
  DataManager::SetValue(FOX_DISABLE_DM_VERITY, "0");
  DataManager::SetValue(FOX_ADVANCED_STOCK_REPLACE, "1");
#endif

#ifdef OF_KEEP_DM_VERITY_FORCED_ENCRYPTION
  DataManager::SetValue(FOX_DISABLE_DM_VERITY, "0");
  DataManager::SetValue(FOX_DISABLE_FORCED_ENCRYPTION, "0");
  DataManager::SetValue(FOX_ADVANCED_STOCK_REPLACE, "1");
#endif

  // LOGINFO("OrangeFox: Reloading theme to apply generated theme on sdcard - again...\n");
  if (DataManager::GetStrValue("used_custom_encryption") == "1")
    PageManager::RequestReload();

  // Launch the main GUI
  gui_start();

#ifndef TW_OEM_BUILD

  // Disable flashing of stock recovery
  TWFunc::Disable_Stock_Recovery_Replace();

  // Check for su to see if the device is rooted or not
  if (DataManager::GetIntValue("tw_mount_system_ro") == 0
      && PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false))
    {
      // read /system/build.prop to get sdk version and do not offer to root if running M or higher (sdk version 23 == M)
      string sdkverstr = TWFunc::System_Property_Get("ro.build.version.sdk");
      int sdkver = 23;
      if (!sdkverstr.empty())
	{
	  sdkver = atoi(sdkverstr.c_str());
	}
      sync();
      PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
    }
#endif

	// Reboot
	TWFunc::Update_Intent_File(Send_Intent);
	delete adb_bu_fifo;
	TWFunc::Update_Log_File();
	gui_msg(Msg("rebooting=Rebooting..."));
	string Reboot_Arg;
	DataManager::GetValue("tw_reboot_arg", Reboot_Arg);
	if (Reboot_Arg == "recovery")
		TWFunc::tw_reboot(rb_recovery);
	else if (Reboot_Arg == "poweroff")
		TWFunc::tw_reboot(rb_poweroff);
	else if (Reboot_Arg == "bootloader")
		TWFunc::tw_reboot(rb_bootloader);
	else if (Reboot_Arg == "download")
		TWFunc::tw_reboot(rb_download);
	else if (Reboot_Arg == "edl")
		TWFunc::tw_reboot(rb_edl);
	else
		TWFunc::tw_reboot(rb_system);

  return 0;
}

