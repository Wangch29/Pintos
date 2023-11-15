#include "threads/tokenizer.h"
#include "threads/malloc.h"
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define TOKEN_MAX_LENGTH 128

struct tokens
  {
    uint8_t tokens_length;
    char** tokens;
  };

/** Push elem into pointer. */
static void*
vector_push(char*** pointer, uint8_t* size, void* elem)
{
  *pointer = (char**) realloc (*pointer, sizeof(char*) * (*size + 1));
  (*pointer)[*size] = elem;
  *size += 1;
  return elem;
}

/** Deep copy a string.
    source is the string pointer, n is the length of string. */
static void*
copy_word(char* source, int8_t n)
{
  source[n] = '\0';
  char* word = (char*) malloc (n + 1);
  strncpy(word, source, n + 1); //TODO: change strncpy to strlcpy in string.c
  strlcpy (word, source, n + 1)
  return word;
}

/** Turn a string to a struct token */
struct tokens*
tokenize(const char* line)
{
  if (line == NULL)
    return NULL;

  static char token[TOKEN_MAX_LENGTH];
  uint8_t n = 0;
  struct tokens* tokens;
  size_t line_length = strlen (line);

  tokens = (struct tokens*) malloc (sizeof(struct tokens));
  tokens->tokens_length = 0;
  tokens->tokens = NULL;

  for (unsigned int i = 0; i < line_length; i++)
    {
      char c = line[i];
      if (isspace(c))
        {
          if (n > 0)
            {
              /* Insert current token into tokens. */
              void* word = copy_word (token, n);
              vector_push (&tokens->tokens, &tokens->tokens_length, word);
              n = 0;
            }
        }
        else
          /* Add char to the next token. */
          token[n++] = c;

      if (n + 1 >= TOKEN_MAX_LENGTH)
        break;
    }
  /* Insert the last word, if any. */
  if (n > 0)
    {
      void* word = copy_word(token, n);
      vector_push(&tokens->tokens, &tokens->tokens_length, word);
    }
  return tokens;
}

/** Return the length of tokens. */
size_t
tokens_get_length(struct tokens* tokens) {
  if (tokens == NULL) {
    return 0;
  } else {
    return tokens->tokens_length;
  }
}

/** Return the nth word in tokens. */
char*
tokens_get_token(struct tokens* tokens, size_t n)
{
  if (tokens == NULL || n >= tokens->tokens_length)
    return NULL;
  else
    return tokens->tokens[n];
}

/** Destroy and free the tokens. */
void
tokens_destroy(struct tokens* tokens)
{
  if (tokens == NULL)
    return;
  for (int i = 0; i < tokens->tokens_length; i++)
    free (tokens->tokens[i]);
  if (tokens->tokens)
    free (tokens->tokens);
  free (tokens);
}