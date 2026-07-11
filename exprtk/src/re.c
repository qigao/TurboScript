/*
 *
 * Mini regex-module inspired by Rob Pike's regex code described in:
 *
 * http://www.cs.princeton.edu/courses/archive/spr09/cos333/beautiful.html
 *
 *
 *
 * Supports:
 * ---------
 *   '.'        Dot, matches any character
 *   '^'        Start anchor, matches beginning of string
 *   '$'        End anchor, matches end of string
 *   '*'        Asterisk, match zero or more (greedy)
 *   '+'        Plus, match one or more (greedy)
 *   '?'        Question, match zero or one (non-greedy)
 *   '[abc]'    Character class, match if one of {'a', 'b', 'c'}
 *   '[^abc]'   Inverted class, match if NOT one of {'a', 'b', 'c'}
 *   '[a-zA-Z]' Character ranges, the character set of the ranges { a-z | A-Z }
 *   'a|b'      Branch, matches either expression
 *   '(a|b)+'   Group, matches grouped expression and allows quantifiers
 *   '\s'       Whitespace, \t \f \r \n \v and spaces
 *   '\S'       Non-whitespace
 *   '\w'       Alphanumeric, [a-zA-Z0-9_]
 *   '\W'       Non-alphanumeric
 *   '\d'       Digits, [0-9]
 *   '\D'       Non-digits
 *
 *
 */



#include "re.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Definitions: */

#define MAX_REGEXP_OBJECTS      30    /* Max number of regex symbols in expression. */
#define MAX_CHAR_CLASS_LEN      40    /* Max length of character-class buffer in.   */
#define MAX_PATTERN_LEN         256   /* Max length of the source pattern buffer.   */


enum { UNUSED, DOT, BEGIN, END, QUESTIONMARK, STAR, PLUS, CHAR, CHAR_CLASS, INV_CHAR_CLASS, DIGIT, NOT_DIGIT, ALPHA, NOT_ALPHA, WHITESPACE, NOT_WHITESPACE, PATTERN_SOURCE };

typedef struct regex_t
{
  unsigned char  type;   /* CHAR, STAR, etc.                      */
  union
  {
    unsigned char  ch;   /*      the character itself             */
    unsigned char* ccl;  /*  OR  a pointer to characters in class */
  } u;
} regex_t;



/* Private function declarations: */
static int matchdigit(char c);
static int matchalpha(char c);
static int matchwhitespace(char c);
static int matchmetachar(char c, const char* str);
static int matchdot(char c);
static int ismetachar(char c);
static int isquantifier(char c);
static int find_char_class_end(const char* pattern, int start, int end);
static int find_group_end(const char* pattern, int start, int end);
static int find_branch_end(const char* pattern, int start, int end);
static int validate_expr(const char* pattern, int start, int end);
static int validate_sequence(const char* pattern, int start, int end);
static int atom_end_at(const char* pattern, int start, int end);
static int matchcharclass_n(char c, const char* str, int len);
static int match_expr(const char* pattern, int start, int end, const char* text,
                      int pos, int text_len, int* out_pos);
static int match_sequence(const char* pattern, int start, int end, const char* text,
                          int pos, int text_len, int* out_pos);
static int match_atom_once(const char* pattern, int start, int atom_end, const char* text,
                           int pos, int text_len, int* out_pos);



/* Public functions: */
int re_match(const char* pattern, const char* text, int* matchlength)
{
  return re_matchp(re_compile(pattern), text, matchlength);
}

int re_matchp(re_t pattern, const char* text, int* matchlength)
{
  regex_t* compiled = (regex_t*)pattern;
  const char* source;
  int pattern_len;
  int text_len;
  int idx;

  if (matchlength == NULL)
  {
    return -1;
  }

  *matchlength = 0;
  if ((compiled != 0) && (compiled[0].type == PATTERN_SOURCE) && (text != NULL))
  {
    source = (const char*)compiled[0].u.ccl;
    pattern_len = (int)strlen(source);
    text_len = (int)strlen(text);

    for (idx = 0; idx <= text_len; ++idx)
    {
      int out_pos = idx;
      if (match_expr(source, 0, pattern_len, text, idx, text_len, &out_pos))
      {
        if (text[idx] == '\0')
        {
          return -1;
        }

        *matchlength = out_pos - idx;
        return idx;
      }

      if (source[0] == '^')
      {
        break;
      }
    }
  }
  return -1;
}

re_t re_compile(const char* pattern)
{
  static regex_t re_compiled[MAX_REGEXP_OBJECTS];
  static unsigned char pattern_buf[MAX_PATTERN_LEN];
  size_t len;

  if (pattern == NULL)
  {
    return 0;
  }

  len = strlen(pattern);
  if (len >= MAX_PATTERN_LEN)
  {
    return 0;
  }

  if (!validate_expr(pattern, 0, (int)len))
  {
    return 0;
  }

  memcpy(pattern_buf, pattern, len + 1);
  re_compiled[0].type = PATTERN_SOURCE;
  re_compiled[0].u.ccl = pattern_buf;
  re_compiled[1].type = UNUSED;

  return (re_t) re_compiled;
}

void re_print(regex_t* pattern)
{
  const char* types[] = { "UNUSED", "DOT", "BEGIN", "END", "QUESTIONMARK", "STAR", "PLUS", "CHAR", "CHAR_CLASS", "INV_CHAR_CLASS", "DIGIT", "NOT_DIGIT", "ALPHA", "NOT_ALPHA", "WHITESPACE", "NOT_WHITESPACE", "PATTERN_SOURCE" };

  int i;
  int j;
  char c;
  for (i = 0; i < MAX_REGEXP_OBJECTS; ++i)
  {
    if (pattern[i].type == UNUSED)
    {
      break;
    }

    printf("type: %s", types[pattern[i].type]);
    if (pattern[i].type == PATTERN_SOURCE)
    {
      printf(" \"%s\"", pattern[i].u.ccl);
    }
    else if (pattern[i].type == CHAR_CLASS || pattern[i].type == INV_CHAR_CLASS)
    {
      printf(" [");
      for (j = 0; j < MAX_CHAR_CLASS_LEN; ++j)
      {
        c = pattern[i].u.ccl[j];
        if ((c == '\0') || (c == ']'))
        {
          break;
        }
        printf("%c", c);
      }
      printf("]");
    }
    else if (pattern[i].type == CHAR)
    {
      printf(" '%c'", pattern[i].u.ch);
    }
    printf("\n");
  }
}



/* Private functions: */
static int matchdigit(char c)
{
  return isdigit((unsigned char)c);
}
static int matchalpha(char c)
{
  return isalpha((unsigned char)c);
}
static int matchwhitespace(char c)
{
  return isspace((unsigned char)c);
}
static int matchalphanum(char c)
{
  return ((c == '_') || matchalpha(c) || matchdigit(c));
}
static int matchdot(char c)
{
#if defined(RE_DOT_MATCHES_NEWLINE) && (RE_DOT_MATCHES_NEWLINE == 1)
  (void)c;
  return 1;
#else
  return c != '\n' && c != '\r';
#endif
}
static int ismetachar(char c)
{
  return ((c == 's') || (c == 'S') || (c == 'w') || (c == 'W') || (c == 'd') || (c == 'D'));
}

static int matchmetachar(char c, const char* str)
{
  switch (str[0])
  {
    case 'd': return  matchdigit(c);
    case 'D': return !matchdigit(c);
    case 'w': return  matchalphanum(c);
    case 'W': return !matchalphanum(c);
    case 's': return  matchwhitespace(c);
    case 'S': return !matchwhitespace(c);
    default:  return (c == str[0]);
  }
}

static int isquantifier(char c)
{
  return (c == '*') || (c == '+') || (c == '?');
}

static int find_char_class_end(const char* pattern, int start, int end)
{
  int i = start + 1;
  if (i < end && pattern[i] == '^')
  {
    i++;
  }
  while (i < end)
  {
    if (pattern[i] == '\\')
    {
      if (i + 1 >= end)
      {
        return -1;
      }
      i += 2;
      continue;
    }
    if (pattern[i] == ']')
    {
      return i;
    }
    i++;
  }
  return -1;
}

static int find_group_end(const char* pattern, int start, int end)
{
  int depth = 1;
  int i;
  for (i = start + 1; i < end; ++i)
  {
    if (pattern[i] == '\\')
    {
      if (i + 1 >= end)
      {
        return -1;
      }
      i++;
      continue;
    }
    if (pattern[i] == '[')
    {
      int class_end = find_char_class_end(pattern, i, end);
      if (class_end < 0)
      {
        return -1;
      }
      i = class_end;
      continue;
    }
    if (pattern[i] == '(')
    {
      depth++;
    }
    else if (pattern[i] == ')')
    {
      depth--;
      if (depth == 0)
      {
        return i;
      }
    }
  }
  return -1;
}

static int find_branch_end(const char* pattern, int start, int end)
{
  int depth = 0;
  int i;
  for (i = start; i < end; ++i)
  {
    if (pattern[i] == '\\')
    {
      if (i + 1 >= end)
      {
        return -1;
      }
      i++;
      continue;
    }
    if (pattern[i] == '[')
    {
      int class_end = find_char_class_end(pattern, i, end);
      if (class_end < 0)
      {
        return -1;
      }
      i = class_end;
      continue;
    }
    if (pattern[i] == '(')
    {
      depth++;
    }
    else if (pattern[i] == ')')
    {
      depth--;
      if (depth < 0)
      {
        return -1;
      }
    }
    else if ((pattern[i] == '|') && (depth == 0))
    {
      return i;
    }
  }
  return end;
}

static int atom_end_at(const char* pattern, int start, int end)
{
  if (start >= end)
  {
    return -1;
  }

  if (pattern[start] == '[')
  {
    int class_end = find_char_class_end(pattern, start, end);
    if (class_end < 0 || class_end - start >= MAX_CHAR_CLASS_LEN)
    {
      return -1;
    }
    return class_end + 1;
  }
  if (pattern[start] == '(')
  {
    int group_end = find_group_end(pattern, start, end);
    if (group_end < 0 || !validate_expr(pattern, start + 1, group_end))
    {
      return -1;
    }
    return group_end + 1;
  }
  if (pattern[start] == '\\')
  {
    return (start + 1 < end) ? start + 2 : -1;
  }
  if ((pattern[start] == '|') || (pattern[start] == ')') || isquantifier(pattern[start]))
  {
    return -1;
  }
  return start + 1;
}

static int validate_sequence(const char* pattern, int start, int end)
{
  int i = start;
  while (i < end)
  {
    int atom_end = atom_end_at(pattern, i, end);
    if (atom_end < 0)
    {
      return 0;
    }

    if ((atom_end < end) && isquantifier(pattern[atom_end]))
    {
      if ((pattern[i] == '^') || (pattern[i] == '$'))
      {
        return 0;
      }
      atom_end++;
    }

    i = atom_end;
  }
  return 1;
}

static int validate_expr(const char* pattern, int start, int end)
{
  int branch_start = start;
  while (branch_start <= end)
  {
    int branch_end = find_branch_end(pattern, branch_start, end);
    if (branch_end < 0)
    {
      return 0;
    }
    if (!validate_sequence(pattern, branch_start, branch_end))
    {
      return 0;
    }
    if (branch_end == end)
    {
      return 1;
    }
    branch_start = branch_end + 1;
  }
  return 1;
}

static int matchcharclass_n(char c, const char* str, int len)
{
  int i = 0;
  while (i < len)
  {
    if ((i + 2 < len) && (c != '-') && (str[i] != '-') && (str[i + 1] == '-') &&
        (c >= str[i]) && (c <= str[i + 2]))
    {
      return 1;
    }
    if (str[i] == '\\')
    {
      if (i + 1 < len && matchmetachar(c, &str[i + 1]))
      {
        return 1;
      }
      if (i + 1 < len && (c == str[i + 1]) && !ismetachar(str[i + 1]))
      {
        return 1;
      }
      i += 2;
      continue;
    }
    if (c == str[i])
    {
      return (c != '-') || (i == 0) || (i == len - 1);
    }
    i++;
  }
  return 0;
}

static int match_atom_once(const char* pattern, int start, int atom_end, const char* text,
                           int pos, int text_len, int* out_pos)
{
  (void)atom_end;
  if (pattern[start] == '^')
  {
    *out_pos = pos;
    return pos == 0;
  }
  if (pattern[start] == '$')
  {
    *out_pos = pos;
    return pos == text_len;
  }
  if (pattern[start] == '(')
  {
    int group_end = find_group_end(pattern, start, atom_end);
    return match_expr(pattern, start + 1, group_end, text, pos, text_len, out_pos);
  }

  if (pos >= text_len)
  {
    return 0;
  }

  if (pattern[start] == '.')
  {
    if (matchdot(text[pos]))
    {
      *out_pos = pos + 1;
      return 1;
    }
    return 0;
  }
  if (pattern[start] == '[')
  {
    int class_end = find_char_class_end(pattern, start, atom_end);
    int inverted = (start + 1 < class_end) && (pattern[start + 1] == '^');
    int class_start = inverted ? start + 2 : start + 1;
    int in_class = matchcharclass_n(text[pos], &pattern[class_start], class_end - class_start);
    if (inverted ? !in_class : in_class)
    {
      *out_pos = pos + 1;
      return 1;
    }
    return 0;
  }
  if (pattern[start] == '\\')
  {
    if (matchmetachar(text[pos], &pattern[start + 1]))
    {
      *out_pos = pos + 1;
      return 1;
    }
    return 0;
  }
  if (text[pos] == pattern[start])
  {
    *out_pos = pos + 1;
    return 1;
  }
  return 0;
}

static int match_sequence(const char* pattern, int start, int end, const char* text,
                          int pos, int text_len, int* out_pos)
{
  int atom_end;
  char quantifier = 0;
  int rest_start;

  if (start >= end)
  {
    *out_pos = pos;
    return 1;
  }

  atom_end = atom_end_at(pattern, start, end);
  if (atom_end < 0)
  {
    return 0;
  }
  rest_start = atom_end;
  if ((rest_start < end) && isquantifier(pattern[rest_start]))
  {
    quantifier = pattern[rest_start++];
  }

  if (quantifier == 0)
  {
    int next_pos;
    return match_atom_once(pattern, start, atom_end, text, pos, text_len, &next_pos) &&
           match_sequence(pattern, rest_start, end, text, next_pos, text_len, out_pos);
  }

  if (quantifier == '?')
  {
    int next_pos;
    if (match_sequence(pattern, rest_start, end, text, pos, text_len, out_pos))
    {
      return 1;
    }
    return match_atom_once(pattern, start, atom_end, text, pos, text_len, &next_pos) &&
           match_sequence(pattern, rest_start, end, text, next_pos, text_len, out_pos);
  }

  {
    int repeat_count = 0;
    int repeat_cap = text_len - pos + 1;
    int* positions = (int*)malloc((size_t)repeat_cap * sizeof(int));
    int cur_pos = pos;
    int ok = 0;
    int min_count = (quantifier == '+') ? 1 : 0;

    if (positions == NULL)
    {
      return 0;
    }

    positions[0] = pos;
    while (repeat_count + 1 < repeat_cap)
    {
      int next_pos;
      if (!match_atom_once(pattern, start, atom_end, text, cur_pos, text_len, &next_pos) ||
          next_pos == cur_pos)
      {
        break;
      }
      cur_pos = next_pos;
      positions[++repeat_count] = cur_pos;
    }

    while (repeat_count >= min_count)
    {
      if (match_sequence(pattern, rest_start, end, text, positions[repeat_count],
                         text_len, out_pos))
      {
        ok = 1;
        break;
      }
      repeat_count--;
    }

    free(positions);
    return ok;
  }
}

static int match_expr(const char* pattern, int start, int end, const char* text,
                      int pos, int text_len, int* out_pos)
{
  int branch_start = start;
  while (branch_start <= end)
  {
    int branch_end = find_branch_end(pattern, branch_start, end);
    if (branch_end < 0)
    {
      return 0;
    }
    if (match_sequence(pattern, branch_start, branch_end, text, pos, text_len, out_pos))
    {
      return 1;
    }
    if (branch_end == end)
    {
      return 0;
    }
    branch_start = branch_end + 1;
  }
  return 0;
}
