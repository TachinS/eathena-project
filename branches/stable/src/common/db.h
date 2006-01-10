/*****************************************************************************\
 *  Copyright (c) Athena Dev Teams - Licensed under GNU GPL                  *
 *  For more information, see LICENCE in the main folder                     *
 *                                                                           *
 *  This file is separated in two sections:                                  *
 *  (1) public typedefs, enums, unions, structures and defines               *
 *  (2) public functions                                                     *
 *                                                                           *
 *  <B>Notes on the release system:</B>                                      *
 *  When destroying the database both the key and the data are released.     *
 *  When adding an entry to the database, if the entry already exists the    *
 *  key is released.                                                         *
 *  When removing an entry from the database the key is released.            *
 *  NOTE: What is actually released is defined by the release function, the  *
 *  functions of the database only ask for the key and/or data to be         *
 *  released.                                                                *
 *                                                                           *
 *  TODO:                                                                    *
 *  - create custom database allocator                                       *
 *  - see what functions need or should be added to the database interface   *
 *  - make the system thread friendly                                        *
 *                                                                           *
 * @version 2.0 build #???# - transition version                             *
 * @author (build #???#) Flavio @ Amazon Project                             *
 * @author (up to Athena build 4706) Athena Dev Teams                        *
 * @encoding US-ASCII                                                        *
\*****************************************************************************/
#ifndef _DB_H_
#define _DB_H_

#include <stdarg.h>

/*****************************************************************************\
 *  (1) Section with public typedefs, enums, unions, structures and defines. *
 *  DB_DELAY_FINAL_CHANGES - TEMP undefine to make all the final chanes.     *
 *  DBRelease    - Enumeration of release options.                           *
 *  DBType       - Enumeration of database types.                            *
 *  DBOptions    - Bitfield enumeration of database options.                 *
 *  DBKey        - Union of used key types.                                  *
 *  DBApply      - Format of functions applyed to the databases.             *
 *  DBMatcher    - Format of matchers used in DBInterface->getall.           *
 *  DBComparator - Format of the comparators used by the databases.          *
 *  DBHasher     - Format of the hashers used by the databases.              *
 *  DBReleaser   - Format of the releasers used by the databases.            *
 *  DBInterface  - Structure of the interface of the database.               *
\*****************************************************************************/

/**
 * Temporary.
 * Delay the final changes not fully compatible with the previous code.
 * @public
 */
#define DB_DELAY_FINAL_CHANGES

/**
 * Bitfield with what should be released by the releaser function (if the
 * function supports it).
 * @public
 * @see #DBReleaser
 * @see #db_custom_release(DBRelease)
 */
typedef enum {
	DB_RELEASE_NOTHING = 0,
	DB_RELEASE_KEY     = 1,
	DB_RELEASE_DATA    = 2,
	DB_RELEASE_BOTH    = 3
} DBRelease;

/**
 * Supported types of database.
 * See {@link #db_fix_options(DBType,DBOptions)} for restrictions of the 
 * types of databases.
 * @param DB_INT Uses int's for keys
 * @param DB_UINT Uses unsigned int's for keys
 * @param DB_STRING Uses strings for keys.
 * @param DB_ISTRING Uses case insensitive strings for keys.
 * @public
 * @see #DBOptions
 * @see #DBKey
 * @see #db_fix_options(DBType,DBOptions)
 * @see #db_default_cmp(DBType)
 * @see #db_default_hash(DBType)
 * @see #db_default_release(DBType,DBOptions)
 * @see #db_alloc(const char *,int,DBType,DBOptions,unsigned short)
 */
typedef enum {
	DB_INT,
	DB_UINT,
	DB_STRING,
	DB_ISTRING
} DBType;

/**
 * Bitfield of options that define the behaviour of the database.
 * See {@link #db_fix_options(DBType,DBOptions)} for restrictions of the 
 * types of databases.
 * @param DB_OPT_BASE Base options: does not duplicate keys, releases nothing
 *          and does not allow NULL keys or NULL data.
 * @param DB_OPT_DUP_KEY Duplicates the keys internally. If DB_OPT_RELEASE_KEY 
 *          is defined, the real key is freed as soon as the entry is added.
 * @param DB_OPT_RELEASE_KEY Releases the key.
 * @param DB_OPT_RELEASE_DATA Releases the data.
 * @param DB_OPT_RELEASE_BOTH Releases both key and data.
 * @param DB_OPT_ALLOW_NULL_KEY Allow NULL keys in the database.
 * @param DB_OPT_ALLOW_NULL_DATA Allow NULL data in the database.
 * @public
 * @see #db_fix_options(DBType,DBOptions)
 * @see #db_default_release(DBType,DBOptions)
 * @see #db_alloc(const char *,int,DBType,DBOptions,unsigned short)
 */
typedef enum {
	DB_OPT_BASE            = 0,
	DB_OPT_DUP_KEY         = 1,
	DB_OPT_RELEASE_KEY     = 2,
	DB_OPT_RELEASE_DATA    = 4,
	DB_OPT_RELEASE_BOTH    = 6,
	DB_OPT_ALLOW_NULL_KEY  = 8,
	DB_OPT_ALLOW_NULL_DATA = 16,
	DB_OPT_RELEASE_DATA_ON_REPLACE = 32
} DBOptions;

/**
 * Union of key types used by the database.
 * @param i Type of key for DB_INT databases
 * @param ui Type of key for DB_UINT databases
 * @param str Type of key for DB_STRING and DB_ISTRING databases
 * @public
 * @see #DBType
 * @see #DBApply(DBKey,void *,va_list)
 * @see #DBMatcher(DBKey,void *,va_list)
 * @see #DBComparator(DBKey,DBKey,unsigned short)
 * @see #DBHasher(DBKey,unsigned short)
 * @see #DBReleaser(DBKey,void *,DBRelease)
 * @see DBInterface#get(DBInterface,DBKey)
 * @see DBInterface#put(DBInterface,DBKey,void *)
 * @see DBInterface#remove(DBInterface,DBKey)
 */
typedef union {
	int i;
	unsigned int ui;
	unsigned char *str;
#ifdef DB_DELAY_FINAL_CHANGES
    // This generic pointer is only included to maintain full compatibility 
	// with the rest of the code that uses the old database system.
	// Without this, all the functions applyed to the database (see DBApply)
	// would throw a compile warning.
	void *p;
#endif /* DB_DELAY_FINAL_CHANGES */
} DBKey __attribute__ ((__transparent_union__));

/**
 * Format of functions to be applyed to an unspecified quantity of entries of 
 * a database.
 * Any function that applyes this function to the database will return the sum 
 * of values returned by this function.
 * @param key Key of the database entry
 * @param data Data of the database entry
 * @param args Extra arguments of the funtion
 * @return Value to be added up by the funtion that is applying this
 * @public
 * @see #DBKey
 * @see DBInterface#foreach(DBInterface,DBApply,...)
 * @see DBInterface#destroy(DBInterface,DBApply,...)
 */
typedef int (*DBApply)(DBKey key, void *data, va_list args);

/**
 * Format of functions that match database entries.
 * The purpose of the match depends on the function that is calling the matcher.
 * Returns 0 if it is a match, another number otherwise.
 * @param key Key of the database entry
 * @param data Data of the database entry
 * @param args Extra arguments of the function
 * @return 0 if a match, another number otherwise
 * @public
 * @see #DBKey
 * @see DBInterface#getall(DBInterface,void **,unsigned int,DBMatcher,...)
 */
typedef int (*DBMatcher)(DBKey key, void *data, va_list args);

/**
 * Format of the comparators used internally by the database system.
 * Compares key1 to key2.
 * <code>maxlen</code> is the maximum number of character used in DB_STRING and 
 * DB_ISTRING databases. If 0, the maximum number of maxlen is used (64K).
 * Returns 0 is equal, negative if lower and positive is higher.
 * @param key1 Key being compared
 * @param key2 Key we are comparing to
 * @param maxlen Maximum number of characters used in DB_STRING and DB_ISTRING
 *          databases.
 * @return 0 if equal, negative if lower and positive if higher
 * @public
 * @see #DBKey
 * @see #db_default_cmp(DBType)
 */
typedef int (*DBComparator)(DBKey key1, DBKey key2, unsigned short maxlen);

/**
 * Format of the hashers used internally by the database system.
 * Creates the hash of the key.
 * <code>maxlen</code> is the maximum number of character used in DB_STRING and 
 * DB_ISTRING databases. If 0, the maximum number of maxlen is used (64K).
 * @param key Key being hashed
 * @param maxlen Maximum number of characters used in DB_STRING and DB_ISTRING
 *          databases.
 * @return Hash of the key
 * @public
 * @see #DBKey
 * @see #db_default_hash(DBType)
 */
typedef unsigned int (*DBHasher)(DBKey key, unsigned short maxlen);

/**
 * Format of the releaser used by the database system.
 * Releases nothing, the key, the data or both.
 * All standard releasers use aFree to release.
 * @param key Key of the database entry
 * @param data Data of the database entry
 * @param which What is being requested to be released
 * @public
 * @see #DBRelease
 * @see #DBKey
 * @see #db_default_releaser(DBType,DBOptions)
 * @see #db_custom_release(DBRelease)
 */
typedef void (*DBReleaser)(DBKey key, void *data, DBRelease which);

/**
 * Public interface of a database. Only contains funtions.
 * All the functions take the interface as the first argument.
 * @public
 * @see DBInterface#get(DBInterface,DBKey)
 * @see DBInterface#getall(DBInterface,void **,unsigned int,DBMatch,...)
 * @see DBInterface#vgetall(DBInterface,void **,unsigned int,DBMatch,va_list)
 * @see DBInterface#put(DBInterface,DBKey,void *)
 * @see DBInterface#remove(DBInterface,DBKey)
 * @see DBInterface#foreach(DBInterface,DBApply,...)
 * @see DBInterface#vforeach(DBInterface,DBApply,va_list)
 * @see DBInterface#destroy(DBInterface,DBApply,...)
 * @see DBInterface#destroy(DBInterface,DBApply,va_list)
 * @see DBInterface#size(DBInterface)
 * @see DBInterface#type(DBInterface)
 * @see DBInterface#options(DBInterface)
 * @see #db_alloc(const char *,int,DBType,DBOptions,unsigned short)
 */
typedef struct dbt {

	/**
	 * Get the data of the entry identifid by the key.
	 * @param dbi Interface of the database
	 * @param key Key that identifies the entry
	 * @return Data of the entry or NULL if not found
	 * @protected
	 * @see #DBKey
	 * @see #DBInterface
	 * @see common\db.c#db_get(DBInterface,DBKey)
	 */
	void *(*get)(struct dbt *dbi, DBKey key);

	/**
	 * Just calls {@link DBInterface#vgetall(DBInterface,void **,unsigned int,DBMatch,va_list)}.
	 * Get the data of the entries matched by <code>match</code>.
	 * It puts a maximum of <code>max</code> entries into <code>buf</code>.
	 * If <code>buf</code> is NULL, it only counts the matches.
	 * Returns the number of entries that matched.
	 * NOTE: if the value returned is greater than <code>max</code>, only the 
	 * first <code>max</code> entries found are put into the buffer.
	 * @param dbi Interface of the database
	 * @param buf Buffer to put the data of the matched entries
	 * @param max Maximum number of data entries to be put into buf
	 * @param match Function that matches the database entries
	 * @param ... Extra arguments for match
	 * @return The number of entries that matched
	 * @protected
	 * @see #DBMatcher(DBKey key, void *data, va_list args)
	 * @see #DBInterface
	 * @see DBInterface#vgetall(DBInterface,void **,unsigned int,DBMatch,va_list)
	 * @see common\db.c#db_getall(DBInterface,void **,unsigned int,DBMatch,...)
	 */
	unsigned int (*getall)(struct dbt *dbi, void **buf, unsigned int max, DBMatcher match, ...);

	/**
	 * Get the data of the entries matched by <code>match</code>.
	 * It puts a maximum of <code>max</code> entries into <code>buf</code>.
	 * If <code>buf</code> is NULL, it only counts the matches.
	 * Returns the number of entries that matched.
	 * NOTE: if the value returned is greater than <code>max</code>, only the 
	 * first <code>max</code> entries found are put into the buffer.
	 * @param dbi Interface of the database
	 * @param buf Buffer to put the data of the matched entries
	 * @param max Maximum number of data entries to be put into buf
	 * @param match Function that matches the database entries
	 * @param ... Extra arguments for match
	 * @return The number of entries that matched
	 * @protected
	 * @see #DBMatcher(DBKey key, void *data, va_list args)
	 * @see #DBInterface
	 * @see DBInterface#getall(DBInterface,void **,unsigned int,DBMatch,...)
	 * @see common\db.c#db_vgetall(DBInterface,void **,unsigned int,DBMatch,va_list)
	 */
	unsigned int (*vgetall)(struct dbt *dbi, void **buf, unsigned int max, DBMatcher match, va_list args);

	/**
	 * Put the data identified by the key in the database.
	 * Returns the previous data if the entry exists or NULL.
	 * NOTE: Uses the new key, the old one is released.
	 * @param dbi Interface of the database
	 * @param key Key that identifies the data
	 * @param data Data to be put in the database
	 * @return The previous data if the entry exists or NULL
	 * @protected
	 * @see #DBKey
	 * @see #DBInterface
	 * @see common\db.c#db_put(DBInterface,DBKey,void *)
	 */
	void *(*put)(struct dbt *dbi, DBKey key, void *data);

	/**
	 * Remove an entry from the database.
	 * Returns the data of the entry.
	 * NOTE: The key (of the database) is released.
	 * @param dbi Interface of the database
	 * @param key Key that identifies the entry
	 * @return The data of the entry or NULL if not found
	 * @protected
	 * @see #DBKey
	 * @see #DBInterface
	 * @see common\db.c#db_remove(DBInterface,DBKey)
	 */
	void *(*remove)(struct dbt *dbi, DBKey key);

	/**
	 * Just calls {@link DBInterface#vforeach(DBInterface,DBApply,va_list)}.
	 * Apply <code>func</code> to every entry in the database.
	 * Returns the sum of values returned by func.
	 * @param dbi Interface of the database
	 * @param func Function to be applyed
	 * @param ... Extra arguments for func
	 * @return Sum of the values returned by func
	 * @protected
	 * @see #DBInterface
	 * @see #DBApply(DBKey,void *,va_list)
	 * @see DBInterface#vforeach(DBInterface,DBApply,va_list)
	 * @see common\db.c#db_foreach(DBInterface,DBApply,...)
	 */
	int (*foreach)(struct dbt *dbi, DBApply func, ...);

	/**
	 * Apply <code>func</code> to every entry in the database.
	 * Returns the sum of values returned by func.
	 * @param dbi Interface of the database
	 * @param func Function to be applyed
	 * @param args Extra arguments for func
	 * @return Sum of the values returned by func
	 * @protected
	 * @see #DBApply(DBKey,void *,va_list)
	 * @see #DBInterface
	 * @see DBInterface#foreach(DBInterface,DBApply,...)
	 * @see common\db.c#db_vforeach(DBInterface,DBApply,va_list)
	 */
	int (*vforeach)(struct dbt *dbi, DBApply func, va_list args);

	/**
	 * Just calls {@link DBInterface#vdestroy(DBInterface,DBApply,va_list)}.
	 * Finalize the database, feeing all the memory it uses.
	 * Before deleting an entry, func is applyed to it.
	 * Releases the key and the data.
	 * Returns the sum of values returned by func, if it exists.
	 * NOTE: This locks the database globally. Any attempt to insert or remove 
	 * a database entry will give an error and be aborted.
	 * @param dbi Interface of the database
	 * @param func Function to be applyed to every entry before deleting
	 * @param ... Extra arguments for func
	 * @return Sum of values returned by func
	 * @protected
	 * @see #DBApply(DBKey,void *,va_list)
	 * @see #DBInterface
	 * @see DBInterface#vdestroy(DBInterface,DBApply,va_list)
	 * @see common\db.c#db_destroy(DBInterface,DBApply,...)
	 */
	int (*destroy)(struct dbt *dbi, DBApply func, ...);

	/**
	 * Finalize the database, feeing all the memory it uses.
	 * Before deleting an entry, func is applyed to it.
	 * Returns the sum of values returned by func, if it exists.
	 * NOTE: This locks the database globally. Any attempt to insert or remove 
	 * a database entry will give an error and be aborted.
	 * @param dbi Interface of the database
	 * @param func Function to be applyed to every entry before deleting
	 * @param args Extra arguments for func
	 * @return Sum of values returned by func
	 * @protected
	 * @see #DBInterface
	 * @see #DBApply(DBKey,void *,va_list)
	 * @see DBInterface#destroy(DBInterface,DBApply,...)
	 * @see common\db.c#db_vdestroy(DBInterface,DBApply,va_list)
	 */
	int (*vdestroy)(struct dbt *dbi, DBApply func, va_list args);

	/**
	 * Return the size of the database (number of items in the database).
	 * @param dbi Interface of the database
	 * @return Size of the database
	 * @protected
	 * @see #DBInterface
	 * @see common\db.c#db_size(DBInterface)
	 */
	unsigned int (*size)(struct dbt *dbi);

	/**
	 * Return the type of the database.
	 * @param dbi Interface of the database
	 * @return Type of the database
	 * @protected
	 * @see #DBType
	 * @see #DBInterface
	 * @see common\db.c#db_type(DBInterface)
	 */
	DBType (*type)(struct dbt *dbi);

	/**
	 * Return the options of the database.
	 * @param dbi Interface of the database
	 * @return Options of the database
	 * @protected
	 * @see #DBOptions
	 * @see #DBInterface
	 * @see common\db.c#db_options(DBInterface)
	 */
	DBOptions (*options)(struct dbt *dbi);

} *DBInterface;

#define strdb_search(db,k)   (db)->get((db),(DBKey)(unsigned char *)(k))
#define strdb_insert(db,k,d) (db)->put((db),(DBKey)(unsigned char *)(k),(unsigned char*)(d))
#define strdb_erase(db,k)    (db)->remove((db),(DBKey)(unsigned char *)(k))
#define strdb_foreach        db_foreach
#define strdb_final          db_destroy
#define strdb_init(a)        db_alloc(__FILE__,__LINE__,DB_STRING,(DB_OPT_ALLOW_NULL_KEY|DB_OPT_ALLOW_NULL_DATA),a)

#define numdb_search(db,k)   (db)->get((db),(DBKey)(int)(k))
#define numdb_insert(db,k,d) (db)->put((db),(DBKey)(int)(k),(void *)(d))
#define numdb_erase(db,k)    (db)->remove((db),(DBKey)(int)(k))
#define numdb_foreach        db_foreach
#define numdb_final          db_destroy
#define numdb_init()         db_alloc(__FILE__,__LINE__,DB_INT,DB_OPT_ALLOW_NULL_DATA,sizeof(int))

#ifdef DB_DELAY_FINAL_CHANGES
#	define exit_dbn db_final
#endif /* DB_DELAY_FINAL_CHANGES */

/*****************************************************************************\
 *  (2) Section with public functions.                                       *
 *  db_fix_options     - Fix the options for a type of database.             *
 *  db_default_cmp     - Get the default comparator for a type of database.  *
 *  db_default_hash    - Get the default hasher for a type of database.      *
 *  db_default_release - Get the default releaser for a type of database     *
 *           with the fixed options.                                         *
 *  db_custom_release  - Get the releaser that behaves as specified.         *
 *  db_alloc           - Allocate a new database.                            *
 *  db_init            - Initialise the database system.                     *
 *  db_final           - Finalise the database system.                       *
\*****************************************************************************/

#ifdef DB_DELAY_FINAL_CHANGES
// To be removed
int db_foreach(DBInterface dbi, DBApply func, ...); // use DBInterface->foreach(DBInterface,DBApply func,...) or DBInterface->vforeach(DBInterface,DBApply func,va_list)
int db_destroy(DBInterface dbi, DBApply func, ...); // old db_final - use DBInterface->destroy(DBInterface,DBApply func,...) or DBInterface->vdestroy(DBInterface,DBApply func,va_list)
#endif /* DB_DELAY_FINAL_CHANGES */

/**
 * Returns the fixed options according to the database type.
 * Sets required options and unsets unsupported options.
 * For numeric databases DB_OPT_DUP_KEY and DB_OPT_RELEASE_KEY are unset.
 * @param type Type of the database
 * @param options Original options of the database
 * @return Fixed options of the database
 * @private
 * @see #DBType
 * @see #DBOptions
 * @see #db_default_release(DBType,DBOptions)
 * @see common\db.c#db_fix_options(DBType,DBOptions)
 */
DBOptions db_fix_options(DBType type, DBOptions options);

/**
 * Returns the default comparator for the type of database.
 * @param type Type of database
 * @return Comparator for the type of database or NULL if unknown database
 * @public
 * @see #DBType
 * @see #DBComparator
 * @see common\db.c#db_default_cmp(DBType)
 */
DBComparator db_default_cmp(DBType type);

/**
 * Returns the default hasher for the specified type of database.
 * @param type Type of database
 * @return Hasher of the type of database or NULL if unknown database
 * @public
 * @see #DBType
 * @see #DBHasher
 * @see common\db.c#db_default_hash(DBType)
 */
DBHasher db_default_hash(DBType type);

/**
 * Returns the default releaser for the specified type of database with the 
 * specified options.
 * NOTE: the options are fixed by {@link #db_fix_options(DBType,DBOptions)}
 * before choosing the releaser * @param type Type of database
 * @param options Options of the database
 * @return Default releaser for the type of database with the fixed options
 * @public
 * @see #DBType
 * @see #DBOptions
 * @see #DBReleaser
 * @see #db_fix_options(DBType,DBOptions)
 * @see #db_custom_release(DBRelease)
 * @see common\db.c#db_default_release(DBType,DBOptions)
 */
DBReleaser db_default_release(DBType type, DBOptions options);

/**
 * Returns the releaser that behaves as <code>which</code> specifies.
 * @param which Defines what the releaser releases
 * @return Releaser for the specified release options
 * @public
 * @see #DBRelease
 * @see #DBReleaser
 * @see #db_default_release(DBType,DBOptions)
 * @see common\db.c#db_custom_release(DBRelease)
 */
DBReleaser db_custom_release(DBRelease which);

/**
 * Allocate a new database of the specified type.
 * It uses the default comparator, hasher and releaser of the specified 
 * database type and fixed options.
 * NOTE: the options are fixed by {@link #db_fix_options(DBType,DBOptions)}
 * before creating the database.
 * @param file File where the database is being allocated
 * @param line Line of the file where the database is being allocated
 * @param type Type of database
 * @param options Options of the database
 * @param maxlen Maximum length of the string to be used as key in string 
 *          databases
 * @return The interface of the database
 * @public
 * @see #DBType
 * @see #DBInterface
 * @see #db_default_cmp(DBType)
 * @see #db_default_hash(DBType)
 * @see #db_default_release(DBType,DBOptions)
 * @see #db_fix_options(DBType,DBOptions)
 * @see common\db.c#db_alloc(const char *,int,DBType,DBOptions,unsigned short)
 */
DBInterface db_alloc(const char *file, int line, DBType type, DBOptions options, unsigned short maxlen);

/**
 * Initialize the database system.
 * @public
 * @see #db_final(void)
 * @see common\db.c#db_init(void)
 */
void db_init(void);

/**
 * Finalize the database system.
 * Frees the memory used by the block reusage system.
 * @public
 * @see #db_init(void)
 * @see common\db.c#db_final(void)
 */
void db_final(void);

#endif
