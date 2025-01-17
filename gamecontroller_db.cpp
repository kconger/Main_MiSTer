#include "gamecontroller_db.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "input.h"
#include "file_io.h"
#include "user_io.h"
#include "profiling.h"




//Note: sdl gamecontrollerdb maps  a,b,x,y differently, so we need to swap each pair (a<->b, x<->y)
static const char *sdlname_to_mister_idx[] = {
	"dpright",
	"dpleft",
	"dpdown",
	"dpup",
	"b",
	"a",
	"y",
	"x",
	"leftshoulder",
	"rightshoulder",
	"back",
	"start",
	"---",
	"---",
	"---",
	"---",
	"---",
	"---",
	"---",
	"---",
	"---", //20
	"guide", //21
	"---",
	"---",
	"leftx",
	"lefty",
	"rightx",
	"righty",
};

typedef struct {
	uint16_t id[4]; //bustype, vid, pid, version
	uint32_t map[NUMBUTTONS];
} controllerdb_entry;




static controllerdb_entry db_maps[MAX_GCDB_ENTRIES] = {};
static int last_db_idx = 0;

//platform should be at the end of mapping strings. this function will null the start of platform: if found
static bool cdb_entry_matches(char *db_str) 
{
	char *pl_ptr = NULL;
	
	if (!db_str || !strlen(db_str)) return false;
	pl_ptr = strcasestr(db_str, "platform:");
	if (!pl_ptr) return false;

	*pl_ptr = 0; 
	pl_ptr += strlen("platform:");
	if (!strncasecmp(pl_ptr, "Linux", 5)) return true;
	if (!strncasecmp(pl_ptr, "MiSTer", 6))
	{
		char *core_ptr = NULL;
		core_ptr = strcasestr(db_str, "mistercore:");
		if (!core_ptr) return true; //platform:MiSTer with no core designators is a generic match
		while(core_ptr)
		{
				char *nxt_c = NULL;
				char *match_c = NULL;
				core_ptr += 11;
				nxt_c = strchr(core_ptr, ',');
				if (!nxt_c) break;
				match_c = strchr(core_ptr, '*');
				if (!match_c || match_c >= nxt_c) 
				{	
					match_c = nxt_c;
				}


				if (!strncasecmp(core_ptr, user_io_get_core_name(), match_c - core_ptr)) return true;
				if (!strncasecmp(core_ptr, user_io_get_core_name(1), match_c - core_ptr)) return true;
				core_ptr = strcasestr(nxt_c, "mistercore:");
		}
	}
	return false;
}

static int find_mister_button_num(char *sdl_name, bool *idx_high)
{
	*idx_high = false;
	for(size_t i = 0; i < sizeof(sdlname_to_mister_idx)/sizeof(char *); i++)
	{
		const char *map_str = sdlname_to_mister_idx[i];
		if (!strcmp(map_str, sdl_name)) return i;
	}
	if (!strcasecmp(sdl_name, "menuok"))
	{
		*idx_high = false;
		return SYS_BTN_MENU_FUNC;
	}

	if (!strcasecmp(sdl_name, "menuesc"))
	{
		*idx_high = true;
		return SYS_BTN_MENU_FUNC;
	}
	return -1;
}


static int find_linux_code_for_button(char *btn_name, uint16_t *btn_map, uint16_t *abs_map)
{
	if (!btn_name || !strlen(btn_name)) return -1;

	switch(btn_name[0])
	{
			case 'b':
			{
				//Normal button
				int bidx = strtol(btn_name+1, NULL, 10);
				return btn_map[bidx]; 
				break;
			}
			case 'a':
			{
				int aidx = strtol(btn_name+1, NULL, 10);
				return abs_map[aidx];
				break;
			}
			case 'h':
			//Mister creates fake digital buttons for hats that depend on the code and axis direction. 
			{
				char *dot_ptr = NULL;
				int hidx = strtol(btn_name+1, NULL, 10);
				int base_hat = ABS_HAT0X + hidx*2;
				dot_ptr = strchr(btn_name, '.');
				if (dot_ptr)
				{
					int hat_dir = strtol(dot_ptr+1, NULL, 10);
					switch(hat_dir)
					{
						case 1:
							return KEY_EMU + ((base_hat+1) << 1);
							break;
						case 2:
							return KEY_EMU + (base_hat << 1) + 1;
							break;
						case 4:
							return KEY_EMU + ((base_hat+1) << 1) + 1;
						case 8:
							return KEY_EMU + (base_hat << 1); 
							break;
						default:
							return -1;
							break;
					}
				}
				break;
			}
			default:
				return -1;
	}
	return -1;
}


#define test_bit(bit, array)  (array [bit / 8] & (1 << (bit % 8)))

static bool parse_mapping_string(char *map_str, char *guid, int dev_fd, uint32_t *fill_map)
{

	static char map_guid[GUID_LEN] = {0};
	static uint16_t btn_map[KEY_MAX - BTN_JOYSTICK] = {0};
	static uint16_t abs_map[ABS_MAX] = {0};

	if (!map_str || !fill_map) return false;


	//gamecontrollerdb references buttons/axes numerically, and the number depends on the actual buttons supported
	//by the controller. Build a map of button number -> keycode (same with axes)

	if (strcmp(map_guid, guid)) //New guid, map out button indexes for this new controller 
	{
		unsigned char keybits[(KEY_MAX+7) / 8];
		unsigned char absbits[(ABS_MAX+7) / 8];
		uint16_t btn_cnt = 0;
		uint16_t abs_cnt = 0;
		bzero(btn_map, sizeof(btn_map));
		bzero(abs_map, sizeof(abs_map));
		printf("Gamecontrollerdb: mapping buttons for %s ", guid);
		if (ioctl(dev_fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0)
		{
			for (int i = BTN_JOYSTICK; i < KEY_MAX; i++)
			{
					if (test_bit(i, keybits))
					{
						printf("b%d->%d ", btn_cnt, i);
						btn_map[btn_cnt] = i;
						btn_cnt++;
					}
			}

			for (int i = 0; i < BTN_JOYSTICK; i++)
			{
					if (test_bit(i, keybits))
					{	
						printf("b%d -> %d ", btn_cnt, i);
						btn_map[btn_cnt] = i;
						btn_cnt++;
					}
			}
			printf("\n");
				
		}

		printf("Gamecontrollerdb: mapping analog axes for %s ", guid);
		if (ioctl(dev_fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0)
		{
			//The "correct" way is to test  all the way to ABS_MAX and skip any hats the device has.
			//Mister handles hats differently and it is unlikely most things in the db files have axes beyond
			//The normal sticks+triggers...
			for (int i = 0; i < ABS_HAT0X; i++)
			{
				if (test_bit(i, absbits))
				{
						printf("a%d->%d ", abs_cnt, i);
						abs_map[abs_cnt] = i;
						abs_cnt++;
				}
			}

			//Just for debugging purposes...
			for (int i = ABS_HAT0X; i < ABS_MAX; i++)
			{
					if (test_bit(i, absbits))
					{
						printf("(debug)a%d->%d ", abs_cnt, i);
						abs_cnt++;
					}
			}
		}
		printf("\n");
	}

	char l_btn[20] = {};
	char m_btn[20] = {};
	bool in_m_btn = true;
	size_t i = 0;
	bool map_parsed = false;
	char *cur_str = map_str; 

	while (cur_str && *cur_str)
	{
		if (*cur_str == ':')
		{
			i = 0;
			in_m_btn = false;
		} else if (*cur_str == ',') {
			i = 0;
			in_m_btn = true;
			if (l_btn[0] && m_btn[0])
			{
				bool m_button_high = false;
				int m_button_num = find_mister_button_num(m_btn, &m_button_high);
				int l_button_code = find_linux_code_for_button(l_btn, btn_map, abs_map);
				if (m_button_num != -1 && l_button_code != -1)
				{

					map_parsed = true;
					fill_map[m_button_num] =  m_button_high ? ((l_button_code << 16) | fill_map[m_button_num]) : ((l_button_code & 0xFFFF)  | fill_map[m_button_num]);
					if (m_button_num == SYS_BTN_OSD_KTGL+1) fill_map[m_button_num+1] = l_button_code; //guide button
					if (m_button_num >= SYS_AXIS1_X && m_button_num <= SYS_AXIS_MX)
					{
						fill_map[m_button_num] = l_button_code | 0x20000;
					}
				}
			}
			bzero(l_btn, sizeof(l_btn));
			bzero(m_btn, sizeof(m_btn));
		} else if (in_m_btn) {
			//Just truncate button names if they are too big
			if (i <= sizeof(m_btn))
			{
				m_btn[i] = *cur_str;
				i++;
			}
		}	 else {
			if (i <= sizeof(l_btn))
			{
				l_btn[i] = *cur_str;
				i++;
			}
		}	
		cur_str++;
	}

	if (map_parsed)
	{
		if ((fill_map[SYS_BTN_MENU_FUNC] & 0xFFFF) == 0)
		{
			fill_map[SYS_BTN_MENU_FUNC] = fill_map[SYS_BTN_A] & 0xFFFF;
		}

		if ((fill_map[SYS_BTN_MENU_FUNC] & 0xFFFF0000) == 0)
		{
			fill_map[SYS_BTN_MENU_FUNC] = (fill_map[SYS_BTN_B] << 16) | fill_map[SYS_BTN_MENU_FUNC];
		}

		if (fill_map[SYS_AXIS_X] == 0) fill_map[SYS_AXIS_X] = fill_map[SYS_AXIS1_X];
		if (fill_map[SYS_AXIS_Y] == 0) fill_map[SYS_AXIS_Y] = fill_map[SYS_AXIS1_Y];
	}


	return map_parsed;
}


#define GCDB_DIR  "/media/fat/linux/gamecontrollerdb/"


bool read_controller_map_from_file(char *fname, char *guid, int dev_fd, uint32_t *fill_map)
{
	fileTextReader reader;
	char matched[1024] = {}; 
	char *map_start = NULL;

	if (FileOpenTextReader(&reader, fname))
	{
		const char *line;
		printf("Gamecontrollerdb: searching for GUID %s in file %s\n", guid, fname);
		while ((line = FileReadLine(&reader)))
		{
			if (line[0] == '#') continue;
			const char *gcom = strchr(line, ',');
			if (!strncasecmp(line, guid, gcom-line))
			{
				if (cdb_entry_matches((char *)gcom))
				{
					map_start = strchr((char *)gcom+1, ',');
					if (map_start)
					{
						strncpy(matched, map_start+1, sizeof(matched));
					}
				}
			}
		}

	}
	if (matched[0] != 0)
	{
		printf("Gamecontrollerdb: found match, using config %s\n", matched);
		return parse_mapping_string(matched, guid, dev_fd, fill_map);	
	}

	return false; 
}

static int gcdb_controller_idx(uint16_t bustype, uint16_t vid, uint16_t pid, uint16_t version)
{
	for (int i=0; i < MAX_GCDB_ENTRIES; i++)
	{
		if (db_maps[i].id[0] == bustype && db_maps[i].id[1] == vid && db_maps[i].id[2] == pid && db_maps[i].id[3] == version)
		{
			return i;
		}
	}
	return -1;
}

static void gcdb_cache_controller_map(uint16_t bustype, uint16_t vid, uint16_t pid, uint16_t version, uint32_t *button_map)
{
	if (gcdb_controller_idx(bustype, vid, pid, version) > -1)
	{
		//Already cached
		return;
	}

	db_maps[last_db_idx].id[0] = bustype;
	db_maps[last_db_idx].id[1] = vid;
	db_maps[last_db_idx].id[2] = pid;
	db_maps[last_db_idx].id[3] = version;
	memcpy(db_maps[last_db_idx].map, button_map, sizeof(uint32_t)*NUMBUTTONS);
	last_db_idx = (last_db_idx +1) % MAX_GCDB_ENTRIES;
}


bool gcdb_map_for_controller(uint16_t bustype, uint16_t vid, uint16_t pid, uint16_t version, int dev_fd, uint32_t *fill_map)
{
		PROFILE_FUNCTION();
		char guid_str[GUID_LEN] = {};
		int cache_idx = gcdb_controller_idx(bustype, vid, pid, version);
		if (cache_idx != -1)
		{
			memcpy(fill_map, db_maps[cache_idx].map, sizeof(uint32_t)*NUMBUTTONS);

			return true;
		}
		sprintf(guid_str, "%04x0000%04x0000%04x0000%04x0000", (uint16_t)(bustype << 8 | bustype >> 8), (uint16_t)( vid << 8 |  vid >> 8), (uint16_t)(pid << 8 | pid >> 8), (uint16_t)(version << 8 | version >> 8));

		char path[256] = {GCDB_DIR};
		strcat(path, "gamecontrollerdb_user.txt");
		bool found_entry = false;
		if (!(found_entry = read_controller_map_from_file(path, guid_str, dev_fd, fill_map)))
		{
			strcpy(path, GCDB_DIR);
			strcat(path, "gamecontrollerdb.txt");
			found_entry = read_controller_map_from_file(path, guid_str, dev_fd, fill_map);
		}
		

		if (found_entry)
		{
			gcdb_cache_controller_map(bustype, vid, pid, version, fill_map);
			return true;
		}
		return false; 
}









