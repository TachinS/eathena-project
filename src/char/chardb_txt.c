// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/db.h"
#include "../common/lock.h"
#include "../common/malloc.h"
#include "../common/mapindex.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/strlib.h"
#include "../common/timer.h"
#include "char.h"
#include "chardb.h"
#include "charlog.h"
#include "int_registry.h"
#include "inter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// temporary stuff
extern int autosave_interval;
extern int parse_friend_txt(struct mmo_charstatus *p);
extern int parse_hotkey_txt(struct mmo_charstatus *p);
extern void mmo_friends_sync(void);
extern void mmo_hotkeys_sync(void);


/// internal structure
typedef struct CharDB_TXT
{
	CharDB vtable;      // public interface

	DBMap* chars;       // in-memory character storage
	int next_char_id;   // auto_increment
	int save_timer;     // save timer id

	char char_db[1024]; // character data storage file

} CharDB_TXT;

/// internal functions
static bool char_db_txt_init(CharDB* self);
static void char_db_txt_destroy(CharDB* self);
static bool char_db_txt_create(CharDB* self, struct mmo_charstatus* status);
static bool char_db_txt_remove(CharDB* self, const int char_id);
static bool char_db_txt_save(CharDB* self, const struct mmo_charstatus* status);
static bool char_db_txt_load_num(CharDB* self, struct mmo_charstatus* ch, int char_id);
static bool char_db_txt_load_str(CharDB* self, struct mmo_charstatus* ch, const char* name);
static bool char_db_txt_load_slot(CharDB* self, struct mmo_charstatus* status, int account_id, int slot);
static bool char_db_txt_id2name(CharDB* self, int char_id, char name[NAME_LENGTH]);
static bool char_db_txt_name2id(CharDB* self, const char* name, int* char_id);
static bool char_db_txt_slot2id(CharDB* self, int account_id, int slot, int* char_id);
int mmo_char_fromstr(CharDB* chars, const char *str, struct mmo_charstatus *p, struct regs* reg);
static int mmo_char_sync_timer(int tid, unsigned int tick, int id, intptr data);
static void mmo_char_sync(CharDB_TXT* db);
int mmo_chars_tobuf(CharDB* chars, struct char_session_data* sd, uint8* buf);

/// public constructor
CharDB* char_db_txt(void)
{
	CharDB_TXT* db = (CharDB_TXT*)aCalloc(1, sizeof(CharDB_TXT));

	// set up the vtable
	db->vtable.init      = &char_db_txt_init;
	db->vtable.destroy   = &char_db_txt_destroy;
	db->vtable.create    = &char_db_txt_create;
	db->vtable.remove    = &char_db_txt_remove;
	db->vtable.save      = &char_db_txt_save;
	db->vtable.load_num  = &char_db_txt_load_num;
	db->vtable.load_str  = &char_db_txt_load_str;
	db->vtable.load_slot = &char_db_txt_load_slot;
	db->vtable.id2name   = &char_db_txt_id2name;
	db->vtable.name2id   = &char_db_txt_name2id;
	db->vtable.slot2id   = &char_db_txt_slot2id;

	// initialize to default values
	db->chars = NULL;
	db->next_char_id = START_CHAR_NUM;
	db->save_timer = INVALID_TIMER;
	// other settings
	safestrncpy(db->char_db, "save/athena.txt", sizeof(db->char_db));

	return &db->vtable;
}


/* ------------------------------------------------------------------------- */


static bool char_db_txt_init(CharDB* self)
{
	CharDB_TXT* db = (CharDB_TXT*)self;
	DBMap* chars;

	char line[65536];
	int line_count = 0;
	int ret;
	FILE* fp;

	// create chars database
	db->chars = idb_alloc(DB_OPT_RELEASE_DATA);
	chars = db->chars;

	// open data file
	fp = fopen(db->char_db, "r");
	if( fp == NULL )
	{
		ShowError("Characters file not found: %s.\n", db->char_db);
		char_log("Characters file not found: %s.\n", db->char_db);
		char_log("Id for the next created character: %d.\n", db->next_char_id);
		return false;
	}

	// load data file
	while( fgets(line, sizeof(line), fp) != NULL )
	{
		int char_id, n;
		struct mmo_charstatus ch;
		struct mmo_charstatus* tmp;
		struct regs reg;
		line_count++;

		if( line[0] == '/' && line[1] == '/' )
			continue;

		n = 0;
		if( sscanf(line, "%d\t%%newid%%%n", &char_id, &n) == 1 && n > 0 && (line[n] == '\n' || line[n] == '\r') )
		{// auto-increment
			if( char_id > db->next_char_id )
				db->next_char_id = char_id;
			continue;
		}

		ret = mmo_char_fromstr(self, line, &ch, &reg);

		// Initialize char regs
		inter_charreg_save(ch.char_id, &reg);
		// Initialize friends list
		parse_friend_txt(&ch);  // Grab friends for the character
		// Initialize hotkey list
		parse_hotkey_txt(&ch);  // Grab hotkeys for the character

		if( ret <= 0 )
		{
			ShowError("mmo_char_init: in characters file, unable to read the line #%d.\n", line_count);
			ShowError("               -> Character saved in log file.\n");
			switch( ret )
			{
			case  0: char_log("Unable to get a character in the next line - Basic structure of line (before inventory) is incorrect (character not readed):\n"); break;
			case -1: char_log("Duplicate character id in the next character line (character not readed):\n"); break;
			case -2: char_log("Duplicate character name in the next character line (character not readed):\n"); break;
			case -3: char_log("Invalid memo point structure in the next character line (character not readed):\n"); break;
			case -4: char_log("Invalid inventory item structure in the next character line (character not readed):\n"); break;
			case -5: char_log("Invalid cart item structure in the next character line (character not readed):\n"); break;
			case -6: char_log("Invalid skill structure in the next character line (character not readed):\n"); break;
			case -7: char_log("Invalid register structure in the next character line (character not readed):\n"); break;
			default: break;
			}
			char_log("%s", line);
			continue;
		}

		// record entry in db
		tmp = (struct mmo_charstatus*)aMalloc(sizeof(struct mmo_charstatus));
		memcpy(tmp, &ch, sizeof(struct mmo_charstatus));
		idb_put(chars, ch.account_id, tmp);

		if( db->next_char_id < ch.char_id)
			db->next_char_id = ch.char_id + 1;

		char_num++;
	}

	// close data file
	fclose(fp);

	ShowStatus("mmo_char_init: %d characters read in %s.\n", char_num, db->char_db);
	char_log("mmo_char_init: %d characters read in %s.\n", char_num, db->char_db);
	char_log("Id for the next created character: %d.\n", db->next_char_id);

	// initialize data saving timer
	add_timer_func_list(mmo_char_sync_timer, "mmo_char_sync_timer");
	db->save_timer = add_timer_interval(gettick() + 1000, mmo_char_sync_timer, 0, (intptr)chars, autosave_interval);

	return true;
}

static void char_db_txt_destroy(CharDB* self)
{
	CharDB_TXT* db = (CharDB_TXT*)self;
	DBMap* chars = db->chars;

	// stop saving timer
	delete_timer(db->save_timer, mmo_char_sync_timer);

	// write data
	mmo_char_sync(db);

	// delete chars database
	chars->destroy(chars, NULL);
	db->chars = NULL;

	// delete entire structure
	aFree(db);
}

static bool char_db_txt_create(CharDB* self, struct mmo_charstatus* status)
{
	CharDB_TXT* db = (CharDB_TXT*)self;

	// flush data
	mmo_char_sync(db);

	return true;
}

static bool char_db_txt_remove(CharDB* self, const int char_id)
{
	return true;
}

static bool char_db_txt_save(CharDB* self, const struct mmo_charstatus* ch)
{
	CharDB_TXT* db = (CharDB_TXT*)self;
	DBMap* chars = db->chars;
	int char_id = ch->char_id;

	// retrieve previous data
	struct mmo_charstatus* tmp = idb_get(chars, char_id);
	if( tmp == NULL )
	{// error condition - entry not found
		return false;
	}
	
	// overwrite with new data
	memcpy(tmp, ch, sizeof(struct mmo_charstatus));

	return true;
}

static bool char_db_txt_load_num(CharDB* self, struct mmo_charstatus* ch, int char_id)
{
	CharDB_TXT* db = (CharDB_TXT*)self;
	DBMap* chars = db->chars;

	// retrieve data
	struct mmo_charstatus* tmp = idb_get(chars, char_id);
	if( tmp == NULL )
	{// entry not found
		return false;
	}

	// store it
	memcpy(ch, tmp, sizeof(struct mmo_charstatus));

	return true;
}

static bool char_db_txt_load_str(CharDB* self, struct mmo_charstatus* ch, const char* name)
{
	CharDB_TXT* db = (CharDB_TXT*)self;
	DBMap* chars = db->chars;

	// retrieve data
	struct DBIterator* iter = chars->iterator(chars);
	struct mmo_charstatus* tmp;

	//TODO: "If exact character name is not found, the function checks without case sensitive and returns index if only 1 character is found"
	for( tmp = (struct mmo_charstatus*)iter->first(iter,NULL); iter->exists(iter); tmp = (struct mmo_charstatus*)iter->next(iter,NULL) )
		if( strcmp(name, tmp->name) == 0 )
			break;
	iter->destroy(iter);

	if( tmp == NULL )
	{// entry not found
		return false;
	}

	// store it
	memcpy(ch, tmp, sizeof(struct mmo_charstatus));

	return true;
}

static bool char_db_txt_load_slot(CharDB* self, struct mmo_charstatus* ch, int account_id, int slot)
{
	CharDB_TXT* db = (CharDB_TXT*)self;
	DBMap* chars = db->chars;

	// retrieve data
	struct DBIterator* iter = chars->iterator(chars);
	struct mmo_charstatus* tmp;

	for( tmp = (struct mmo_charstatus*)iter->first(iter,NULL); iter->exists(iter); (struct mmo_charstatus*)tmp = iter->next(iter,NULL) )
		if( account_id == tmp->account_id && slot == tmp->slot )
			break;
	iter->destroy(iter);

	if( tmp == NULL )
	{// entry not found
		return false;
	}

	// store it
	memcpy(ch, tmp, sizeof(struct mmo_charstatus));

	return true;
}

static bool char_db_txt_id2name(CharDB* self, int char_id, char name[NAME_LENGTH])
{
	return true;
}

static bool char_db_txt_name2id(CharDB* self, const char* name, int* char_id)
{
//	ARR_FIND( 0, char_num, i,
//		(name_ignoring_case && strncmp(char_dat[i].name, name, NAME_LENGTH) == 0) ||
//		(!name_ignoring_case && strncmpi(char_dat[i].name, name, NAME_LENGTH) == 0) );
//	if( i < char_num )

	return true;
}

static bool char_db_txt_slot2id(CharDB* self, int account_id, int slot, int* char_id)
{
	return true;
}


//-------------------------------------------------------------------------
// Function to set the character from the line (at read of characters file)
//-------------------------------------------------------------------------
int mmo_char_fromstr(CharDB* chars, const char *str, struct mmo_charstatus *p, struct regs* reg)
{
	char tmp_str[3][128]; //To avoid deleting chars with too long names.
	int tmp_int[256];
	unsigned int tmp_uint[2]; //To read exp....
	int next, len, i, j;
	struct mmo_charstatus tmp;

	// initilialise character
	memset(p, '\0', sizeof(struct mmo_charstatus));
	
// Char structure of version 1500 (homun + mapindex maps)
	if (sscanf(str, "%d\t%d,%d\t%127[^\t]\t%d,%d,%d\t%u,%u,%d\t%d,%d,%d,%d\t%d,%d,%d,%d,%d,%d\t%d,%d"
		"\t%d,%d,%d\t%d,%d,%d,%d\t%d,%d,%d\t%d,%d,%d,%d,%d"
		"\t%d,%d,%d\t%d,%d,%d,%d,%d,%d,%d,%d%n",
		&tmp_int[0], &tmp_int[1], &tmp_int[2], tmp_str[0],
		&tmp_int[3], &tmp_int[4], &tmp_int[5],
		&tmp_uint[0], &tmp_uint[1], &tmp_int[8],
		&tmp_int[9], &tmp_int[10], &tmp_int[11], &tmp_int[12],
		&tmp_int[13], &tmp_int[14], &tmp_int[15], &tmp_int[16], &tmp_int[17], &tmp_int[18],
		&tmp_int[19], &tmp_int[20],
		&tmp_int[21], &tmp_int[22], &tmp_int[23], //
		&tmp_int[24], &tmp_int[25], &tmp_int[26], &tmp_int[44],
		&tmp_int[27], &tmp_int[28], &tmp_int[29],
		&tmp_int[30], &tmp_int[31], &tmp_int[32], &tmp_int[33], &tmp_int[34],
		&tmp_int[45], &tmp_int[35], &tmp_int[36],
		&tmp_int[46], &tmp_int[37], &tmp_int[38], &tmp_int[39], 
		&tmp_int[40], &tmp_int[41], &tmp_int[42], &tmp_int[43], &next) != 48)
	{
	tmp_int[44] = 0; //Hom ID.
// Char structure of version 1488 (fame field addition)
	if (sscanf(str, "%d\t%d,%d\t%127[^\t]\t%d,%d,%d\t%u,%u,%d\t%d,%d,%d,%d\t%d,%d,%d,%d,%d,%d\t%d,%d"
		"\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d,%d,%d"
		"\t%127[^,],%d,%d\t%127[^,],%d,%d,%d,%d,%d,%d,%d%n",
		&tmp_int[0], &tmp_int[1], &tmp_int[2], tmp_str[0],
		&tmp_int[3], &tmp_int[4], &tmp_int[5],
		&tmp_uint[0], &tmp_uint[1], &tmp_int[8],
		&tmp_int[9], &tmp_int[10], &tmp_int[11], &tmp_int[12],
		&tmp_int[13], &tmp_int[14], &tmp_int[15], &tmp_int[16], &tmp_int[17], &tmp_int[18],
		&tmp_int[19], &tmp_int[20],
		&tmp_int[21], &tmp_int[22], &tmp_int[23], //
		&tmp_int[24], &tmp_int[25], &tmp_int[26],
		&tmp_int[27], &tmp_int[28], &tmp_int[29],
		&tmp_int[30], &tmp_int[31], &tmp_int[32], &tmp_int[33], &tmp_int[34],
		tmp_str[1], &tmp_int[35], &tmp_int[36],
		tmp_str[2], &tmp_int[37], &tmp_int[38], &tmp_int[39], 
		&tmp_int[40], &tmp_int[41], &tmp_int[42], &tmp_int[43], &next) != 47)
	{
	tmp_int[43] = 0; //Fame
// Char structure of version 1363 (family data addition)
	if (sscanf(str, "%d\t%d,%d\t%127[^\t]\t%d,%d,%d\t%u,%u,%d\t%d,%d,%d,%d\t%d,%d,%d,%d,%d,%d\t%d,%d"
		"\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d,%d,%d"
		"\t%127[^,],%d,%d\t%127[^,],%d,%d,%d,%d,%d,%d%n",
		&tmp_int[0], &tmp_int[1], &tmp_int[2], tmp_str[0], //
		&tmp_int[3], &tmp_int[4], &tmp_int[5],
		&tmp_uint[0], &tmp_uint[1], &tmp_int[8],
		&tmp_int[9], &tmp_int[10], &tmp_int[11], &tmp_int[12],
		&tmp_int[13], &tmp_int[14], &tmp_int[15], &tmp_int[16], &tmp_int[17], &tmp_int[18],
		&tmp_int[19], &tmp_int[20],
		&tmp_int[21], &tmp_int[22], &tmp_int[23], //
		&tmp_int[24], &tmp_int[25], &tmp_int[26],
		&tmp_int[27], &tmp_int[28], &tmp_int[29],
		&tmp_int[30], &tmp_int[31], &tmp_int[32], &tmp_int[33], &tmp_int[34],
		tmp_str[1], &tmp_int[35], &tmp_int[36], //
		tmp_str[2], &tmp_int[37], &tmp_int[38], &tmp_int[39], 
		&tmp_int[40], &tmp_int[41], &tmp_int[42], &next) != 46)
	{
	tmp_int[40] = 0; // father
	tmp_int[41] = 0; // mother
	tmp_int[42] = 0; // child
// Char structure version 1008 (marriage partner addition)
	if (sscanf(str, "%d\t%d,%d\t%127[^\t]\t%d,%d,%d\t%u,%u,%d\t%d,%d,%d,%d\t%d,%d,%d,%d,%d,%d\t%d,%d"
		"\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d,%d,%d"
		"\t%127[^,],%d,%d\t%127[^,],%d,%d,%d%n",
		&tmp_int[0], &tmp_int[1], &tmp_int[2], tmp_str[0], //
		&tmp_int[3], &tmp_int[4], &tmp_int[5],
		&tmp_uint[0], &tmp_uint[1], &tmp_int[8],
		&tmp_int[9], &tmp_int[10], &tmp_int[11], &tmp_int[12],
		&tmp_int[13], &tmp_int[14], &tmp_int[15], &tmp_int[16], &tmp_int[17], &tmp_int[18],
		&tmp_int[19], &tmp_int[20],
		&tmp_int[21], &tmp_int[22], &tmp_int[23], //
		&tmp_int[24], &tmp_int[25], &tmp_int[26],
		&tmp_int[27], &tmp_int[28], &tmp_int[29],
		&tmp_int[30], &tmp_int[31], &tmp_int[32], &tmp_int[33], &tmp_int[34],
		tmp_str[1], &tmp_int[35], &tmp_int[36], //
		tmp_str[2], &tmp_int[37], &tmp_int[38], &tmp_int[39], &next) != 43)
	{
	tmp_int[39] = 0; // partner id
// Char structure version 384 (pet addition)
	if (sscanf(str, "%d\t%d,%d\t%127[^\t]\t%d,%d,%d\t%u,%u,%d\t%d,%d,%d,%d\t%d,%d,%d,%d,%d,%d\t%d,%d"
		"\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d\t%d,%d,%d,%d,%d"
		"\t%127[^,],%d,%d\t%127[^,],%d,%d%n",
		&tmp_int[0], &tmp_int[1], &tmp_int[2], tmp_str[0], //
		&tmp_int[3], &tmp_int[4], &tmp_int[5],
		&tmp_uint[0], &tmp_uint[1], &tmp_int[8],
		&tmp_int[9], &tmp_int[10], &tmp_int[11], &tmp_int[12],
		&tmp_int[13], &tmp_int[14], &tmp_int[15], &tmp_int[16], &tmp_int[17], &tmp_int[18],
		&tmp_int[19], &tmp_int[20],
		&tmp_int[21], &tmp_int[22], &tmp_int[23], //
		&tmp_int[24], &tmp_int[25], &tmp_int[26],
		&tmp_int[27], &tmp_int[28], &tmp_int[29],
		&tmp_int[30], &tmp_int[31], &tmp_int[32], &tmp_int[33], &tmp_int[34],
		tmp_str[1], &tmp_int[35], &tmp_int[36], //
		tmp_str[2], &tmp_int[37], &tmp_int[38], &next) != 42)
	{
	tmp_int[26] = 0; // pet id
// Char structure of a version 1 (original data structure)
	if (sscanf(str, "%d\t%d,%d\t%127[^\t]\t%d,%d,%d\t%u,%u,%d\t%d,%d,%d,%d\t%d,%d,%d,%d,%d,%d\t%d,%d"
		"\t%d,%d,%d\t%d,%d\t%d,%d,%d\t%d,%d,%d,%d,%d"
		"\t%127[^,],%d,%d\t%127[^,],%d,%d%n",
		&tmp_int[0], &tmp_int[1], &tmp_int[2], tmp_str[0], //
		&tmp_int[3], &tmp_int[4], &tmp_int[5],
		&tmp_uint[0], &tmp_uint[1], &tmp_int[8],
		&tmp_int[9], &tmp_int[10], &tmp_int[11], &tmp_int[12],
		&tmp_int[13], &tmp_int[14], &tmp_int[15], &tmp_int[16], &tmp_int[17], &tmp_int[18],
		&tmp_int[19], &tmp_int[20],
		&tmp_int[21], &tmp_int[22], &tmp_int[23], //
		&tmp_int[24], &tmp_int[25], //
		&tmp_int[27], &tmp_int[28], &tmp_int[29],
		&tmp_int[30], &tmp_int[31], &tmp_int[32], &tmp_int[33], &tmp_int[34],
		tmp_str[1], &tmp_int[35], &tmp_int[36], //
		tmp_str[2], &tmp_int[37], &tmp_int[38], &next) != 41)
	{
		ShowError("Char-loading: Unrecognized character data version, info lost!\n");
		ShowDebug("Character info: %s\n", str);
		return 0;
	}
	}	// Char structure version 384 (pet addition)
	}	// Char structure version 1008 (marriage partner addition)
	}	// Char structure of version 1363 (family data addition)
	}	// Char structure of version 1488 (fame field addition)
	//Convert save data from string to integer for older formats
		tmp_int[45] = mapindex_name2id(tmp_str[1]);
		tmp_int[46] = mapindex_name2id(tmp_str[2]);
	}	// Char structure of version 1500 (homun + mapindex maps)

	memcpy(p->name, tmp_str[0], NAME_LENGTH); //Overflow protection [Skotlex]
	p->char_id = tmp_int[0];
	p->account_id = tmp_int[1];
	p->slot = tmp_int[2];
	p->class_ = tmp_int[3];
	p->base_level = tmp_int[4];
	p->job_level = tmp_int[5];
	p->base_exp = tmp_uint[0];
	p->job_exp = tmp_uint[1];
	p->zeny = tmp_int[8];
	p->hp = tmp_int[9];
	p->max_hp = tmp_int[10];
	p->sp = tmp_int[11];
	p->max_sp = tmp_int[12];
	p->str = tmp_int[13];
	p->agi = tmp_int[14];
	p->vit = tmp_int[15];
	p->int_ = tmp_int[16];
	p->dex = tmp_int[17];
	p->luk = tmp_int[18];
	p->status_point = min(tmp_int[19], USHRT_MAX);
	p->skill_point = min(tmp_int[20], USHRT_MAX);
	p->option = tmp_int[21];
	p->karma = tmp_int[22];
	p->manner = tmp_int[23];
	p->party_id = tmp_int[24];
	p->guild_id = tmp_int[25];
	p->pet_id = tmp_int[26];
	p->hair = tmp_int[27];
	p->hair_color = tmp_int[28];
	p->clothes_color = tmp_int[29];
	p->weapon = tmp_int[30];
	p->shield = tmp_int[31];
	p->head_top = tmp_int[32];
	p->head_mid = tmp_int[33];
	p->head_bottom = tmp_int[34];
	p->last_point.x = tmp_int[35];
	p->last_point.y = tmp_int[36];
	p->save_point.x = tmp_int[37];
	p->save_point.y = tmp_int[38];
	p->partner_id = tmp_int[39];
	p->father = tmp_int[40];
	p->mother = tmp_int[41];
	p->child = tmp_int[42];
	p->fame = tmp_int[43];
	p->hom_id = tmp_int[44];
	p->last_point.map = tmp_int[45];
	p->save_point.map = tmp_int[46];

#ifndef TXT_SQL_CONVERT
	// Some checks
	//TODO: just a check is needed here (loading all data is excessive)
	if( chars->load_num(chars, &tmp, p->char_id) )
	{
		ShowError(CL_RED"mmmo_auth_init: a character has an identical id to another.\n");
		ShowError("               character id #%d -> new character not readed.\n", p->char_id);
		ShowError("               Character saved in log file."CL_RESET"\n");
		return -1;
	}
	if( chars->load_str(chars, &tmp, p->name) )
	{
		ShowError(CL_RED"mmmo_auth_init: a character name already exists.\n");
		ShowError("               character name '%s' -> new character not read.\n", p->name);
		ShowError("               Character saved in log file."CL_RESET"\n");
		return -2;
	}

	if (strcmpi(wisp_server_name, p->name) == 0) {
		ShowWarning("mmo_auth_init: ******WARNING: character name has wisp server name.\n");
		ShowWarning("               Character name '%s' = wisp server name '%s'.\n", p->name, wisp_server_name);
		ShowWarning("               Character readed. Suggestion: change the wisp server name.\n");
		char_log("mmo_auth_init: ******WARNING: character name has wisp server name: Character name '%s' = wisp server name '%s'.\n",
		          p->name, wisp_server_name);
	}
#endif //TXT_SQL_CONVERT
	if (str[next] == '\n' || str[next] == '\r')
		return 1;	// �V�K�f�[�^

	next++;

	for(i = 0; str[next] && str[next] != '\t'; i++) {
		//mapindex memo format
		if (sscanf(str+next, "%d,%d,%d%n", &tmp_int[2], &tmp_int[0], &tmp_int[1], &len) != 3)
		{	//Old string-based memo format.
			if (sscanf(str+next, "%[^,],%d,%d%n", tmp_str[0], &tmp_int[0], &tmp_int[1], &len) != 3)
				return -3;
			tmp_int[2] = mapindex_name2id(tmp_str[0]);
		}
		if (i < MAX_MEMOPOINTS)
	  	{	//Avoid overflowing (but we must also read through all saved memos)
			p->memo_point[i].x = tmp_int[0];
			p->memo_point[i].y = tmp_int[1];
			p->memo_point[i].map = tmp_int[2];
		}
		next += len;
		if (str[next] == ' ')
			next++;
	}

	next++;

	for(i = 0; str[next] && str[next] != '\t'; i++) {
		if(sscanf(str + next, "%d,%d,%d,%d,%d,%d,%d%[0-9,-]%n",
		      &tmp_int[0], &tmp_int[1], &tmp_int[2], &tmp_int[3],
		      &tmp_int[4], &tmp_int[5], &tmp_int[6], tmp_str[0], &len) == 8)
		{
			p->inventory[i].id = tmp_int[0];
			p->inventory[i].nameid = tmp_int[1];
			p->inventory[i].amount = tmp_int[2];
			p->inventory[i].equip = tmp_int[3];
			p->inventory[i].identify = tmp_int[4];
			p->inventory[i].refine = tmp_int[5];
			p->inventory[i].attribute = tmp_int[6];

			for(j = 0; j < MAX_SLOTS && tmp_str[0] && sscanf(tmp_str[0], ",%d%[0-9,-]",&tmp_int[0], tmp_str[0]) > 0; j++)
				p->inventory[i].card[j] = tmp_int[0];

			next += len;
			if (str[next] == ' ')
				next++;
		} else // invalid structure
			return -4;
	}
	next++;

	for(i = 0; str[next] && str[next] != '\t'; i++) {
		if(sscanf(str + next, "%d,%d,%d,%d,%d,%d,%d%[0-9,-]%n",
		      &tmp_int[0], &tmp_int[1], &tmp_int[2], &tmp_int[3],
		      &tmp_int[4], &tmp_int[5], &tmp_int[6], tmp_str[0], &len) == 8)
		{
			p->cart[i].id = tmp_int[0];
			p->cart[i].nameid = tmp_int[1];
			p->cart[i].amount = tmp_int[2];
			p->cart[i].equip = tmp_int[3];
			p->cart[i].identify = tmp_int[4];
			p->cart[i].refine = tmp_int[5];
			p->cart[i].attribute = tmp_int[6];
			
			for(j = 0; j < MAX_SLOTS && tmp_str && sscanf(tmp_str[0], ",%d%[0-9,-]",&tmp_int[0], tmp_str[0]) > 0; j++)
				p->cart[i].card[j] = tmp_int[0];
			
			next += len;
			if (str[next] == ' ')
				next++;
		} else // invalid structure
			return -5;
	}

	next++;

	for(i = 0; str[next] && str[next] != '\t'; i++) {
		if (sscanf(str + next, "%d,%d%n", &tmp_int[0], &tmp_int[1], &len) != 2)
			return -6;
		p->skill[tmp_int[0]].id = tmp_int[0];
		p->skill[tmp_int[0]].lv = tmp_int[1];
		next += len;
		if (str[next] == ' ')
			next++;
	}

	next++;

	// parse character regs
	if( !inter_charreg_fromstr(str + next, reg) )
		return -7;

	return 1;
}

//-------------------------------------------------
// Function to create the character line (for save)
//-------------------------------------------------
int mmo_char_tostr(char *str, struct mmo_charstatus *p, const struct regs* reg)
{
	int i,j;
	char *str_p = str;

	// character data
	str_p += sprintf(str_p,
		"%d\t%d,%d\t%s\t%d,%d,%d\t%u,%u,%d" //Up to Zeny field
		"\t%d,%d,%d,%d\t%d,%d,%d,%d,%d,%d\t%d,%d" //Up to Skill Point
		"\t%d,%d,%d\t%d,%d,%d,%d" //Up to hom id
		"\t%d,%d,%d\t%d,%d,%d,%d,%d" //Up to head bottom
		"\t%d,%d,%d\t%d,%d,%d" //last point + save point
		",%d,%d,%d,%d,%d\t",	//Family info
		p->char_id, p->account_id, p->slot, p->name, //
		p->class_, p->base_level, p->job_level,
		p->base_exp, p->job_exp, p->zeny,
		p->hp, p->max_hp, p->sp, p->max_sp,
		p->str, p->agi, p->vit, p->int_, p->dex, p->luk,
		p->status_point, p->skill_point,
		p->option, p->karma, p->manner,	//
		p->party_id, p->guild_id, p->pet_id, p->hom_id,
		p->hair, p->hair_color, p->clothes_color,
		p->weapon, p->shield, p->head_top, p->head_mid, p->head_bottom,
		p->last_point.map, p->last_point.x, p->last_point.y, //
		p->save_point.map, p->save_point.x, p->save_point.y,
		p->partner_id,p->father,p->mother,p->child,p->fame);
	for(i = 0; i < MAX_MEMOPOINTS; i++)
		if (p->memo_point[i].map) {
			str_p += sprintf(str_p, "%d,%d,%d ", p->memo_point[i].map, p->memo_point[i].x, p->memo_point[i].y);
		}
	*(str_p++) = '\t';

	// inventory
	for(i = 0; i < MAX_INVENTORY; i++)
		if (p->inventory[i].nameid) {
			str_p += sprintf(str_p,"%d,%d,%d,%d,%d,%d,%d",
				p->inventory[i].id,p->inventory[i].nameid,p->inventory[i].amount,p->inventory[i].equip,
				p->inventory[i].identify,p->inventory[i].refine,p->inventory[i].attribute);
			for(j=0; j<MAX_SLOTS; j++)
				str_p += sprintf(str_p,",%d",p->inventory[i].card[j]);
			str_p += sprintf(str_p," ");
		}
	*(str_p++) = '\t';

	// cart
	for(i = 0; i < MAX_CART; i++)
		if (p->cart[i].nameid) {
			str_p += sprintf(str_p,"%d,%d,%d,%d,%d,%d,%d",
				p->cart[i].id,p->cart[i].nameid,p->cart[i].amount,p->cart[i].equip,
				p->cart[i].identify,p->cart[i].refine,p->cart[i].attribute);
			for(j=0; j<MAX_SLOTS; j++)
				str_p += sprintf(str_p,",%d",p->cart[i].card[j]);
			str_p += sprintf(str_p," ");
		}
	*(str_p++) = '\t';

	// skills
	for(i = 0; i < MAX_SKILL; i++)
		if (p->skill[i].id && p->skill[i].flag != 1) {
			str_p += sprintf(str_p, "%d,%d ", p->skill[i].id, (p->skill[i].flag == 0) ? p->skill[i].lv : p->skill[i].flag-2);
		}
	*(str_p++) = '\t';

	// registry
	if( reg != NULL )
		str_p += inter_charreg_tostr(str_p, reg);
	*(str_p++) = '\t';

	*str_p = '\0';
	return 0;
}

/// Dumps the entire char db (+ associated data) to disk
static void mmo_char_sync(CharDB_TXT* db)
{
	int lock;
	FILE *fp;
	void* data;
	struct DBIterator* iter;

	// Data save
	fp = lock_fopen(db->char_db, &lock);
	if( fp == NULL )
	{
		ShowWarning("Server cannot save characters.\n");
		char_log("WARNING: Server cannot save characters.\n");
		return;
	}

	iter = db->chars->iterator(db->chars);
	for( data = iter->first(iter,NULL); iter->exists(iter); data = iter->next(iter,NULL) )
	{
		struct mmo_charstatus* ch = (struct mmo_charstatus*) data;
		char line[65536]; // ought to be big enough
		struct regs reg;

		inter_charreg_load(ch->char_id, &reg);
		mmo_char_tostr(line, ch, &reg);
		fprintf(fp, "%s\n", line);
	}
	fprintf(fp, "%d\t%%newid%%\n", db->next_char_id);
	iter->destroy(iter);

	lock_fclose(fp, db->char_db, &lock);

	// save associated data
	mmo_friends_sync();

#ifdef HOTKEY_SAVING
	mmo_hotkeys_sync();
#endif
}

/// Periodic data saving function
int mmo_char_sync_timer(int tid, unsigned int tick, int id, intptr data)
{
	CharDB_TXT* db = (CharDB_TXT*)data;

	if (save_log)
		ShowInfo("Saving all files...\n");

	mmo_char_sync(db);
	inter_save();
	return 0;
}

















int mmo_chars_tobuf(CharDB* chars, struct char_session_data* sd, uint8* buf)
{
	struct mmo_charstatus cd;
	int i;

	// TODO: iterate over all chars on account instead of bruteforcing
	chars->load_slot(chars, &cd, sd->account_id, i);

//	for(i = 0; i < found_num; i++)
//		j += mmo_char_tobuf(WFIFOP(fd,j), &char_dat[sd->found_char[i]]);

}




/*

extern DBMap* auth_db;
struct online_char_data {
	int account_id;
	int char_id;
	int fd;
	int waiting_disconnect;
	short server; // -2: unknown server, -1: not connected, 0+: id of server
};
extern DBMap* online_char_db;
extern bool name_ignoring_case;

extern int mmo_friends_list_data_str(char *str, struct mmo_charstatus *p);
extern int mmo_hotkeys_tostr(char *str, struct mmo_charstatus *p);
extern int parse_friend_txt(struct mmo_charstatus *p);
extern int parse_hotkey_txt(struct mmo_charstatus *p);
extern void mmo_hotkeys_sync(void);
extern void mmo_friends_sync(void);
extern int autosave_interval;



//TODO:
// - search char data by account id (multiple results)
// - search char data by char id
// - search char data by account id and char id

void mmo_char_sync(void);




//Search character data from the aid/cid givem
struct mmo_charstatus* search_character(int aid, int cid)
{
	int i;
	for (i = 0; i < char_num; i++) {
		if (char_dat[i].char_id == cid && char_dat[i].account_id == aid)
			return &char_dat[i];
	}
	return NULL;
}
	
struct mmo_charstatus* search_character_byname(char* character_name)
{
	int i = search_character_index(character_name);
	if (i == -1) return NULL;
	return &char_dat[i];
}

//----------------------------------------------
// Search an character id
//   (return character index or -1 (if not found))
//   If exact character name is not found,
//   the function checks without case sensitive
//   and returns index if only 1 character is found
//   and similar to the searched name.
//----------------------------------------------
int search_character_index(char* character_name)
{
	int i, quantity, index;

	quantity = 0;
	index = -1;
	for(i = 0; i < char_num; i++) {
		// Without case sensitive check (increase the number of similar character names found)
		if (stricmp(char_dat[i].name, character_name) == 0) {
			// Strict comparison (if found, we finish the function immediatly with correct value)
			if (strcmp(char_dat[i].name, character_name) == 0)
				return i;
			quantity++;
			index = i;
		}
	}
	// Here, the exact character name is not found
	// We return the found index of a similar account ONLY if there is 1 similar character
	if (quantity == 1)
		return index;

	// Exact character name is not found and 0 or more than 1 similar characters have been found ==> we say not found
	return -1;
}

//Loads a character's name and stores it in the buffer given (must be NAME_LENGTH in size)
//Returns 1 on found, 0 on not found (buffer is filled with Unknown char name)
int char_loadName(int char_id, char* name)
{
	int j;
	for( j = 0; j < char_num && char_dat[j].char_id != char_id; ++j )
		;// find char
	if( j < char_num )
		strncpy(name, char_dat[j].name, NAME_LENGTH);
	else
		strncpy(name, unknown_char_name, NAME_LENGTH);

	return (j < char_num) ? 1 : 0;
}

//Clears the given party id from all characters.
//Since sometimes the party format changes and parties must be wiped, this 
//method is required to prevent stress during the "party not found!" stages.
void char_clearparty(int party_id)
{
	int i;
	for(i = 0; i < char_num; i++)
  	{
		if (char_dat[i].party_id == party_id)
			char_dat[i].party_id = 0;
	}
}



int char_married(int pl1,int pl2)
{
	return (char_dat[pl1].char_id == char_dat[pl2].partner_id && char_dat[pl2].char_id == char_dat[pl1].partner_id);
}

int char_child(int parent_id, int child_id)
{
	return (char_dat[parent_id].child == char_dat[child_id].char_id && 
		((char_dat[parent_id].char_id == char_dat[child_id].father) || 
		(char_dat[parent_id].char_id == char_dat[child_id].mother)));		
}

int char_family(int cid1, int cid2, int cid3)
{
	struct mmo_charstatus cd1, cd2, cd3;
	if( !chars->get_num(chars, &cd1, cid1) || !chars->get_num(chars, &cd2, cid2) || !chars->get_num(chars, &cd3, cid3) )
		return 0; //Some character not found??

	//Unless the dbs are corrupted, these 3 checks should suffice, even though 
	//we could do a lot more checks and force cross-reference integrity.
	if( cd1.partner_id == cid2 && cd1.child == cid3 )
		return cid3; //cid1/cid2 parents. cid3 child.

	if( cd1.partner_id == cid3 && cd1.child == cid2 )
		return cid2; //cid1/cid3 parents. cid2 child.

	if( cd2.partner_id == cid3 && cd2.child == cid1 )
		return cid1; //cid2/cid3 parents. cid1 child.

	return 0;
}
*/
