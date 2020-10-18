/* GNU m4 -- A simple macro processor

   Copyright (C) 1989-1994, 2004-2011 Free Software Foundation, Inc.

   This file is part of GNU M4.

   GNU M4 is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU M4 is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* We use <config.h> instead of "config.h" so that a compilation
   using -I. -I$srcdir will use ./config.h rather than $srcdir/config.h
   (which it would do because it found this file in $srcdir).  */

#ifndef __M4_H // {
#define __M4_H
//#include <config.h>
#define PACKAGE_STRING "M4"
#define RENAME_OPEN_FILE_WORKS 1

//#include <assert.h>
//#include <ctype.h>
//#include <errno.h>
//#include <limits.h>
//#include <stdbool.h>
//#include <stdint.h>
//#include <string.h>
//#include <sys/stat.h>
//#include <sys/types.h>
#include "binary-io.h"
#include "clean-temp.h"
#include "cloexec.h"
//#include "close-stream.h"
#include "closein.h"
//#include "dirname.h"
//#include "error.h"
#include "exitfail.h"
//#include "filenamecat.h"
#include "obstack.h"
#include "stdio--.h"
#include "stdlib--.h"
#include "unistd--.h"
#include "verror.h"
//#include "xalloc.h"
//#include "xprintf.h"
//#include "xvasprintf.h"

/* Canonicalize UNIX recognition macros.  */
#if defined unix || defined __unix || defined __unix__ || defined _POSIX_VERSION || defined _POSIX2_VERSION || defined __NetBSD__ || \
	defined __OpenBSD__ || defined __APPLE__ || defined __APPLE_CC__
	#define UNIX 1
#endif
/* Canonicalize Windows recognition macros.  */
#if (defined _WIN32 || defined __WIN32__) && !defined __CYGWIN__
	#define W32_NATIVE 1
#endif
/* Canonicalize OS/2 recognition macro.  */
#ifdef __EMX__
	#define OS2 1
	#undef UNIX
#endif

#define EXIT_INTERNAL_ERROR 2 /* Used if any programmer error is detected (not possible, right?)  */
#define EXIT_MISMATCH 63 /* Used for version mismatch, when -R detects a frozen file it can't parse.  */
#define _(ARG) ARG /* No-op, for future gettext compatibility.  */

/* Various declarations.  */

struct string {
	char * string;          /* characters of the string */
	size_t length;          /* length of the string */
};

typedef struct string STRING;

/* Memory allocation.  */
#define obstack_chunk_alloc     xmalloc
#define obstack_chunk_free      free

/* Those must come first.  */
typedef struct token_data token_data;
typedef void builtin_func (struct obstack *, int, token_data **);

/* Gnulib's stdbool doesn't work with bool bitfields.  For nicer
   debugging, use bool when we know it works, but use the more
   portable uint elsewhere.  */
#if __GNUC__ > 2
typedef bool bool_bitfield;
#else
typedef uint bool_bitfield;
#endif /* ! __GNUC__ */
/* Take advantage of GNU C compiler source level optimization hints,
   using portable macros.  */
#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 6)
	#define M4_GNUC_ATTRIBUTE(args)        __attribute__ (args)
#else
	#define M4_GNUC_ATTRIBUTE(args)
#endif  /* __GNUC__ */
#define M4_GNUC_UNUSED           M4_GNUC_ATTRIBUTE((__unused__))
#define M4_GNUC_PRINTF(fmt, arg) M4_GNUC_ATTRIBUTE((__format__(__printf__, fmt, arg)))
#define M4_GNUC_NORETURN         M4_GNUC_ATTRIBUTE((__noreturn__))

/* File: m4.c  --- global definitions.  */

/* Option flags.  */
extern int sync_output;                 /* -s */
extern int debug_level;                 /* -d */
extern size_t hash_table_size;          /* -H */
extern int no_gnu_extensions;           /* -G */
extern int prefix_all_builtins;         /* -P */
extern int max_debug_argument_length;   /* -l */
extern int suppress_warnings;           /* -Q */
extern int warning_status;              /* -E */
extern int nesting_limit;               /* -L */
#ifdef ENABLE_CHANGEWORD
extern const char * user_word_regexp;            /* -W */
#endif

/* Error handling.  */
extern int retcode;
//extern const char * program_name;
const char * get_program_name();

void m4_error(int, int, const char *, ...) M4_GNUC_PRINTF(3, 4);
void m4_error_at_line(int, int, const char *, int, const char *, ...) M4_GNUC_PRINTF(5, 6);

#define M4ERROR(Arglist) (m4_error Arglist)
#define M4ERROR_AT_LINE(Arglist) (m4_error_at_line Arglist)

/* File: debug.c  --- debugging and tracing function.  */

extern FILE * _debug;

/* The value of debug_level is a bitmask of the following.  */

#define DEBUG_TRACE_ARGS 1 /* a: show arglist in trace output */
#define DEBUG_TRACE_EXPANSION 2 /* e: show expansion in trace output */
#define DEBUG_TRACE_QUOTE 4 /* q: quote args and expansion in trace output */
#define DEBUG_TRACE_ALL 8 /* t: trace all macros -- overrides trace{on,off} */
#define DEBUG_TRACE_LINE 16 /* l: add line numbers to trace output */
#define DEBUG_TRACE_FILE 32 /* f: add file name to trace output */
#define DEBUG_TRACE_PATH 64 /* p: trace path search of include files */
#define DEBUG_TRACE_CALL 128 /* c: show macro call before args collection */
#define DEBUG_TRACE_INPUT 256 /* i: trace changes of input files */
#define DEBUG_TRACE_CALLID 512 /* x: add call id to trace output */
#define DEBUG_TRACE_VERBOSE 1023 /* V: very verbose --  print everything */
#define DEBUG_TRACE_DEFAULT 7 /* default flags -- equiv: aeq */

#define DEBUG_PRINT1(Fmt, Arg1) \
	do {                                 \
		if(_debug)                       \
			xfprintf(_debug, Fmt, Arg1); \
	} while(0)

#define DEBUG_PRINT3(Fmt, Arg1, Arg2, Arg3) \
	do {                                             \
		if(_debug)                                   \
			xfprintf(_debug, Fmt, Arg1, Arg2, Arg3); \
	} while(0)

#define DEBUG_MESSAGE(Fmt) \
	do {                            \
		if(_debug) {                \
			debug_message_prefix(); \
			xfprintf(_debug, Fmt);  \
			putc('\n', _debug);     \
		}                           \
	} while(0)

#define DEBUG_MESSAGE1(Fmt, Arg1) \
	do {                                 \
		if(_debug) {                     \
			debug_message_prefix();      \
			xfprintf(_debug, Fmt, Arg1); \
			putc('\n', _debug);          \
		}                                \
	} while(0)

#define DEBUG_MESSAGE2(Fmt, Arg1, Arg2) \
	do {                                                           \
		if(_debug != NULL) {                                       \
			debug_message_prefix();                              \
			xfprintf(_debug, Fmt, Arg1, Arg2);                    \
			putc('\n', _debug);                                   \
		}                                                       \
	} while(0)

void debug_init(void);
int debug_decode(const char *);
void debug_flush_files(void);
bool debug_set_output(const char *);
void debug_message_prefix(void);
void trace_prepre(const char *, int);
void trace_pre(const char *, int, int, token_data **);
void trace_post(const char *, int, int, const char *);

/* File: input.c  --- lexical definitions.  */

/* Various different token types.  */
enum token_type {
	TOKEN_EOF,              /* end of file */
	TOKEN_STRING,           /* a quoted string or comment */
	TOKEN_WORD,             /* an identifier */
	TOKEN_OPEN,             /* ( */
	TOKEN_COMMA,            /* , */
	TOKEN_CLOSE,            /* ) */
	TOKEN_SIMPLE,           /* any other single character */
	TOKEN_MACDEF            /* a macro's definition (see "defn") */
};

/* The data for a token, a macro argument, and a macro definition.  */
enum token_data_type {
	TOKEN_VOID,
	TOKEN_TEXT,
	TOKEN_FUNC
};

struct token_data {
	enum token_data_type type;
	union {
		struct {
			char * text;
#ifdef ENABLE_CHANGEWORD
			char * original_text;
#endif
		}

		u_t;

		builtin_func * func;
	}

	u;
};

#define TOKEN_DATA_TYPE(Td)             ((Td)->type)
#define TOKEN_DATA_TEXT(Td)             ((Td)->u.u_t.text)
#ifdef ENABLE_CHANGEWORD
	#define TOKEN_DATA_ORIG_TEXT(Td)       ((Td)->u.u_t.original_text)
#endif
#define TOKEN_DATA_FUNC(Td)             ((Td)->u.func)

typedef enum token_type token_type;
typedef enum token_data_type token_data_type;

void input_init(void);
token_type peek_token(void);
token_type next_token(token_data *, int *);
void skip_line(void);

/* push back input */
void push_file(FILE *, const char *, bool);
void push_macro(builtin_func *);
struct obstack * push_string_init(void);
const char * push_string_finish(void);
void push_wrapup(const char *);
bool pop_wrapup(void);

/* current input file, and line */
extern const char * current_file;
extern int current_line;

/* left and right quote, begin and end comment */
extern STRING bcomm, ecomm;
extern STRING lquote, rquote;

#define DEF_LQUOTE "`"
#define DEF_RQUOTE "\'"
#define DEF_BCOMM "#"
#define DEF_ECOMM "\n"

void set_quotes(const char *, const char *);
void set_comment(const char *, const char *);
#ifdef ENABLE_CHANGEWORD
void set_word_regexp(const char *);
#endif

/* File: output.c --- output functions.  */
extern int current_diversion;
extern int output_current_line;

void output_init(FILE* out);
void output_exit(void);
void output_text(const char *, int);
void shipout_text(struct obstack *, const char *, int, int);
void make_diversion(int);
void insert_diversion(int);
void insert_file(FILE *);
void freeze_diversions(FILE *);

/* File symtab.c  --- symbol table definitions.  */

/* Operation modes for lookup_symbol ().  */
enum symbol_lookup {
	SYMBOL_LOOKUP,
	SYMBOL_INSERT,
	SYMBOL_DELETE,
	SYMBOL_PUSHDEF,
	SYMBOL_POPDEF
};

//
// Symbol table entry.
//
struct SymbolTableEntry {
	SymbolTableEntry * next;
	bool_bitfield traced : 1;
	bool_bitfield shadowed : 1;
	bool_bitfield macro_args : 1;
	bool_bitfield blind_no_args : 1;
	bool_bitfield deleted : 1;
	int pending_expansions;
	char * name;
	token_data data;
};

#define SYMBOL_NEXT(S)          ((S)->next)
#define SYMBOL_TRACED(S)        ((S)->traced)
#define SYMBOL_SHADOWED(S)      ((S)->shadowed)
#define SYMBOL_MACRO_ARGS(S)    ((S)->macro_args)
#define SYMBOL_BLIND_NO_ARGS(S) ((S)->blind_no_args)
#define SYMBOL_DELETED(S)       ((S)->deleted)
#define SYMBOL_PENDING_EXPANSIONS(S) ((S)->pending_expansions)
#define SYMBOL_NAME(S)          ((S)->name)
#define SYMBOL_TYPE(S)          (TOKEN_DATA_TYPE(&(S)->data))
#define SYMBOL_TEXT(S)          (TOKEN_DATA_TEXT(&(S)->data))
#define SYMBOL_FUNC(S)          (TOKEN_DATA_FUNC(&(S)->data))

typedef enum symbol_lookup symbol_lookup;
//typedef struct symbol symbol;
typedef void hack_symbol (SymbolTableEntry *, void *);

#define HASHMAX 509             /* default, overridden by -Hsize */

extern SymbolTableEntry ** symtab;

void free_symbol(SymbolTableEntry * sym);
void symtab_init(void);
SymbolTableEntry * FASTCALL lookup_symbol(const char *, symbol_lookup);
void hack_all_symbols(hack_symbol *, void *);

/* File: macro.c  --- macro expansion.  */

void expand_input(void);
void call_macro(SymbolTableEntry *, int, token_data **, struct obstack *);

/* File: builtin.c  --- builtins.  */

struct builtin {
	const char * name;
	bool_bitfield gnu_extension : 1;
	bool_bitfield groks_macro_args : 1;
	bool_bitfield blind_if_no_args : 1;
	builtin_func * func;
};

struct predefined {
	const char * unix_name;
	const char * gnu_name;
	const char * func;
};

typedef struct builtin builtin;
typedef struct predefined predefined;
struct re_pattern_buffer;

struct re_registers;

/* The default sequence detects multi-digit parameters (obsolete after
   1.4.x), and any use of extended arguments with the default ${}
   syntax (new in 2.0).  */
#define DEFAULT_MACRO_SEQUENCE "\\$\\({[^}]*}\\|[0-9][0-9]+\\)"

void builtin_init(void);
void define_builtin(const char *, const builtin *, symbol_lookup);
void set_macro_sequence(const char *);
void free_macro_sequence(void);
void define_user_macro(const char *, const char *, symbol_lookup);
void undivert_all(void);
void expand_user_macro(struct obstack *, SymbolTableEntry *, int, token_data **);
void m4_placeholder(struct obstack *, int, token_data **);
void init_pattern_buffer(struct re_pattern_buffer *, struct re_registers *);
const char * ntoa(int32_t, int);
const builtin * find_builtin_by_addr(builtin_func *);
const builtin * find_builtin_by_name(const char *);

/* File: path.c  --- path search for include files.  */

void include_init(void);
void include_env_init(void);
void add_include_directory(const char *);
FILE * m4_path_search(const char *, char **);

/* File: eval.c  --- expression evaluation.  */

bool evaluate(const char *, int32_t *);

/* File: format.c  --- printf like formatting.  */
void expand_format(struct obstack *, int, token_data **);

/* File: freeze.c --- frozen state files.  */
void produce_frozen_state(const char *);
void reload_frozen_state(const char *);

/* Debugging the memory allocator.  */
#ifdef WITH_DMALLOC
	#define DMALLOC_FUNC_CHECK
	#include <dmalloc.h>
#endif
/* Other debug stuff.  */
#ifdef DEBUG
	#define DEBUG_INCL   1
	#define DEBUG_INPUT  1
	#define DEBUG_MACRO  1
	#define DEBUG_OUTPUT 1
	#define DEBUG_STKOVF 1
	#define DEBUG_SYM    1
#endif
/* Convert a possibly-signed character to an unsigned character.  This is
   a bit safer than casting to uchar, since it catches some type
   errors that the cast doesn't.  */
#if HAVE_INLINE
static inline uchar to_uchar(char ch) {
	return ch;
}

#else
	#define to_uchar(C) ((uchar)(C))
#endif
#define STREQ(a, b) (strcmp(a, b) == 0) /* Avoid negative logic when comparing two strings.  */
#endif // } __M4_H