#include <ctype.h>
#include <complex.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>

#define LEN(x) sizeof((x))/sizeof((x)[0])
#define EPSILON 1e-8
#define CLOSE(x,y) fabs((x)-(y)) < EPSILON
#define ISZERO(x) CLOSE((x), 0)
#define ALIGNMENT 16 	// Might change that in the future
#define ALIGN_ALLOC(offset, align) ((offset + ((align) - 1)) & (~((align) - 1))) // Alignment trick

#define UNREACHABLE(x) do { fprintf(stderr, "Unreachable: " x); exit(1); } while(0);
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#define MIN(x,y) ((x) < (y) ? (x) : (y))

#define ABERTH_TOL 1e-12
#define ABERTH_MAX_ITER 1000

//============================== 
//
//    Arena
//
//============================== 

typedef struct {
	char *base;
	size_t offset;
	size_t capacity;
} Arena;

Arena arena_create(size_t capacity){
	Arena a;
	a.base     = malloc(capacity);
	a.offset   = 0;
	a.capacity = capacity;
	return a;
}

void arena_destroy(Arena *a){
	free(a->base);
}

void *arena_alloc(Arena *a, size_t size){
	size_t aligned_offset = ALIGN_ALLOC(a->offset, ALIGNMENT);
	if(aligned_offset + size > a->capacity){
		fprintf(stderr, "Arena capacity exceeded. Currently at %zu, requested %zu", a->offset, size);
		return NULL;
	}
	a->offset = aligned_offset + size;
	memset(a->base + aligned_offset, 0, size);
	return a->base + aligned_offset;
}

size_t arena_save(Arena *a){
	return a->offset;
}

void arena_restore(Arena *a, size_t offset){
	a->offset = offset;
}

#define CONCAT_INTERNAL(x,y) x##y
#define CONCAT(x, y) CONCAT_INTERNAL(x, y)
#define SCRATCH_SCOPE(arena) for(size_t CONCAT(scratch_, __LINE__) = arena_save(arena), _once = 1; _once == 1; arena_restore(arena, CONCAT(scratch_, __LINE__)), _once = 0)

//============================== 
//
//    Data structures
//
//============================== 

typedef enum {
  FN_NATIVE,
  FN_USER
} FunctionKind;

struct AST;

typedef struct {
  int         native_index;
  size_t      num_arguments_native;
} NativeFunctionData;

typedef struct {
  FunctionKind kind; 
  union {
    struct AST*        definition;
    NativeFunctionData native_data;
  };
} Function;

int function_repr(Arena *a, size_t num_args, const char *id, char* buf){
	int printed = sprintf(buf, "%s: fn(%zu)", id, num_args);
	buf[printed] = '\0';
  return printed;
}

typedef struct {
	double* nums;
	size_t  size;
} Array;

typedef struct Value Value;
typedef struct {
	Value*  items;
	size_t size;
} Tuple;


// Example: 3x2 - 5x + 1:
// coeffs     = [1, -5, 3]
// num_coeffs = 3
// deg        = 2
typedef struct {
	double* coeffs;
	size_t  num_coeffs;
	size_t  deg;
} Polynomial;

Polynomial alloc_poly(Arena *a, size_t deg){
	Polynomial p;
	p.num_coeffs = deg + 1;
	p.deg        = deg;
	p.coeffs     = arena_alloc(a, p.num_coeffs * sizeof(double));
	memset(p.coeffs, 0, p.num_coeffs * sizeof(double));
	return p;
}

int poly_repr(Arena *a, Polynomial *p, char* buf, char* variable){
	int printed = 0;
  if(variable == NULL){
    variable = "x";
  }
	for(size_t i = 0; i < p->num_coeffs; i++){
		double coef = p->coeffs[i];
		if(coef == 0) continue;
		if(i > 1){
			printed += sprintf(buf + printed, "%.2e %s^%zu", coef, variable, i);
		}else if(i == 1){
			printed += sprintf(buf + printed, "%.2e %s", coef, variable);
		}else{
			printed += sprintf(buf + printed, "%.2e", coef);
		}
		if(i < p->num_coeffs - 1) printed += sprintf(buf + printed, " + ");
	}
	buf[printed] = '\0';
  return printed;
}

// Example: 3x2 - 5x + 1:
// coeffs     = [1, -5, 3]
// num_coeffs = 3
// deg        = 2
typedef struct {
	Polynomial num;
	Polynomial den;
} TransferFunction;

int transfer_function_repr(Arena *a, TransferFunction *tf, char* buf, char* variable){
  char num_buf[1024];
  char den_buf[1024];
  int num_size = poly_repr(a, &tf->num, num_buf, variable);
  int den_size = poly_repr(a, &tf->den, den_buf, variable);

  int max_size     = MAX(num_size, den_size);
  int num_dashes   = max_size;
  int padding_num  = (int)((max_size - num_size)/2);
  int padding_den  = (int)((max_size - den_size)/2);
  int apply_to_num = max_size == num_size ? 0 : 1;

  char padding_buf[1024] = {0};
  char dashes[1024]      = {0};
  for(int i = 0; i < MAX(padding_num, padding_den); i++){
    padding_buf[i] = ' ';
  }
  for(int i = 0; i < max_size; i++){
    dashes[i] = '-';
  }

  size_t printed = 0;

  printed += sprintf(buf + printed, "%s%s\n", apply_to_num ? padding_buf : "", num_buf);
  printed += sprintf(buf + printed, "%s\n", dashes);
  printed += sprintf(buf + printed, "%s%s", apply_to_num ? "" : padding_buf, den_buf);

  buf[printed] = '\0';
  return printed;

}


char *alloc_string(Arena *a, size_t len){
	char* new_string = arena_alloc(a, len + 1);
	memset(new_string, 0, len + 1);
	return new_string;
}


//============================== 
//
//    Tokenization
//
//============================== 

// X-Macros for defining token types.
// I wanted more granular control over how those
// tokens were printed, so instead of defining
// a single X macro, I added a V macro for tokens
// that carry value information

#define TOK_TYPE_LIST \
	V(TOK_KEYWORD) \
	V(TOK_ID) \
	V(TOK_NL) \
	V(TOK_EOF) \
	V(TOK_STRING) \
	V(TOK_NUMBER) \
	X(TOK_LPAREN) \
	X(TOK_RPAREN) \
	X(TOK_LBRACKET) \
	X(TOK_RBRACKET) \
	X(TOK_LCURLY) \
	X(TOK_RCURLY) \
	X(TOK_COMMA) \
	X(TOK_EQ) \
	X(TOK_DBL_EQ) \
	X(TOK_LT) \
	X(TOK_GT) \
	X(TOK_LEQ) \
	X(TOK_GEQ) \
	V(TOK_PLUS) \
	V(TOK_MINUS) \
	V(TOK_MUL) \
	V(TOK_DIV) \
	V(TOK_CARET) \
	V(TOK_AT) \
	X(TOK_TYPE_COUNT)

typedef enum {
#define X(name) name,
#define V(name) name,
	TOK_TYPE_LIST
#undef X
#undef V
} TokenType;

typedef struct {
	TokenType type;
	char      *value;
} Token;

void token_type_repr(TokenType type, char *buf){
	int repr_size = 0;
	switch(type){
#define X(name) case name: { repr_size = sprintf(buf, #name); break;}
#define V(name) case name: { repr_size = sprintf(buf, #name); break;}
		TOK_TYPE_LIST
#undef X
#undef V
	}
	buf[repr_size] = '\0';
}

void token_repr(Token *tok, char *buf){
	int repr_size = 0;
	switch(tok->type){
#define X(name) case name: { repr_size = sprintf(buf, #name); break;}
#define V(name) case name: { repr_size = sprintf(buf, "%-15s [%s]", #name, tok->value); break;}
		TOK_TYPE_LIST
#undef X
#undef V
	}
	buf[repr_size] = '\0';
}

void token_print(Token *tok){
	char buf[256];
	token_repr(tok, buf);
	printf("%s\n", buf);
}

typedef struct {
	const char   *source;
	size_t       size;
	size_t       offset;
} Tokenizer;

int tokenizer_eat_char(Tokenizer *tokenizer, char c){
  const char *pos = tokenizer->source + tokenizer->offset;
  if(pos[0] == c){
    tokenizer->offset++;
    return 0;
  }
  return 1;
}


size_t tokenizer_is_id(const char *pos){
	if (!isalpha((unsigned char)pos[0]) && pos[0] != '_')
		return 0;
	size_t i = 1;
	while(isalnum((unsigned char)pos[i]) || pos[i] == '_'){
		i += 1;
	}
	return i;
}

size_t tokenizer_is_number(const char *pos){
	if(!isdigit(pos[0])) return 0;
	size_t i = 1;
	while(isdigit(pos[i])){
		i += 1;
	}
	if(pos[i] == '.' && isdigit(pos[i + 1])){
		// scientific notation
		i += 1;
	}
	while(isdigit(pos[i])){
		i += 1;
	}
  if((pos[i] == 'e' || pos[i] == 'E') &&
     (isdigit(pos[i + 1]) ||
      ((pos[i + 1] == '+' || pos[i + 1] == '-') && isdigit(pos[i + 2])))){
          i += (pos[i + 1] == '+' || pos[i + 1] == '-') ? 2 : 1;
          while(isdigit(pos[i])) i += 1;
  }       
	return i;
}


int tokenizer_lookahead(const char *pos, const char *symbol){
	if(strncmp(pos + 1, symbol, 1) == 0) return 1;
	return 0;
}
int tokenizer_is_symbol(const char *pos, const char *symbol){
	if(strncmp(pos, symbol, 1) == 0) return strlen(symbol);
	return 0;
}

int tokenizer_is_str(const char *pos, const char *str, size_t n){
	if(strncmp(pos, str, n) == 0) return 1;
	return 0;
}

Token tokenizer_make_symbol(Arena *a, TokenType type, Tokenizer *tokenizer, size_t symbol_len){
	Token tok = {
		tok.type  = type,
		tok.value = NULL,
	};
	size_t num_chars = symbol_len;
	tokenizer->offset += num_chars;
	return tok;
}

int tokenizer_eat_string(Arena *a, Tokenizer *tokenizer, Token *out){
	tokenizer_eat_char(tokenizer, '"');
	size_t first = tokenizer->offset;

	while(tokenizer->offset < tokenizer->size &&
			tokenizer->source[tokenizer->offset] != '"'  &&
			tokenizer->source[tokenizer->offset] != '\n'){
		tokenizer->offset++;
	}

	if(tokenizer->offset >= tokenizer->size || tokenizer->source[tokenizer->offset] != '"'){
		fprintf(stderr, "Unterminated string at offset %zu\n", first - 1);
		return 1;
	}

	size_t num_chars = tokenizer->offset - first;
	tokenizer_eat_char(tokenizer, '"');

	out->type  = TOK_STRING;
	out->value = arena_alloc(a, num_chars + 1);
	memcpy(out->value, tokenizer->source + first, num_chars);
	out->value[num_chars] = '\0';
	return 0;
}

#define KEYWORDS_LIST  \
        X(let)         \
        X(fn)          \
        X(if)          \
        X(else)        \
        X(elif)        \
        X(return)      \
        V(KEYWORDS_COUNT)

typedef enum {
#define X(x) kw_##x,
#define V(x) x,
  KEYWORDS_LIST
#undef X
#undef V
} Keyword;

const char *KEYWORDS[KEYWORDS_COUNT] = {
#define X(x) #x,
#define V(x)
  KEYWORDS_LIST
#undef X
#undef V
};

size_t keywork_match(const char *pos){
  for(size_t i = 0; i < KEYWORDS_COUNT; i++){
    size_t len = strlen(KEYWORDS[i]);
    if(
        tokenizer_is_str(pos, KEYWORDS[i], len)
        && !isalpha((unsigned char)pos[len])
        && pos[len] != '_'
      )
    {
      return len;
    }
  }
  return 0;
}

Token tokenize_keyword(Arena *a, const char *pos, size_t len){
  char *value = arena_alloc(a, len + 1);
  strncpy(value, pos, len);
  value[len] = '\0';
  Token tok = {
    .type  = TOK_KEYWORD,
    .value = value,
  };
  return tok;
}



int tokenize(Arena *a, const char *line, size_t n, Token *tokens , size_t *num_tokens){
	Tokenizer tokenizer;
	tokenizer.source = line;
	tokenizer.offset = 0;
	tokenizer.size   = n;

	*num_tokens      = 0;
	// printf("Tokenizing: %s\n", line);

	while(tokenizer.offset < tokenizer.size){
		const char *pos = tokenizer.source + tokenizer.offset;

    if(pos[0] == '\n'){
        Token token = {
          .type = TOK_NL,
          .value = "\\n",
        };
        tokens[(*num_tokens)++] = token;
        tokenizer.offset++;
        continue;
    }

		if(pos[0] == '/' && pos[1] == '/'){
      while(tokenizer.offset < tokenizer.size && tokenizer.source[tokenizer.offset] != '\n') {
        tokenizer.offset++;
      }
      continue;
		}

		if(isspace(pos[0])){
			tokenizer.offset++;
			continue;
		}

    // Check for strings
    
    if(pos[0] == '"'){
      Token tok;
			if(tokenizer_eat_string(a, &tokenizer, &tok) != 0) return 1;
      tokens[(*num_tokens)++] = tok;
      continue;
    }

		// Check for keywords
    size_t keyword_len = keywork_match(pos);
    if(keyword_len > 0){
      Token tok = tokenize_keyword(a, pos, keyword_len);
			tokens[*num_tokens] = tok;
			(*num_tokens)++;
			tokenizer.offset += keyword_len;
      continue;
    }

		// Check for identifiers
		int id_size = tokenizer_is_id(pos);
		if(id_size > 0){
			char *value = arena_alloc(a, id_size + 1);
			strncpy(value, pos, id_size);
			value[id_size] = '\0';
			Token tok = {
				tok.type  = TOK_ID,
				tok.value = value,
			};
			tokens[*num_tokens] = tok;
			(*num_tokens)++;
			tokenizer.offset += id_size;
			// token_print(&tok);
			continue;
		}

		// Check for identifiers
		int num_size = tokenizer_is_number(pos);
		if(num_size > 0){
			char *value = arena_alloc(a, num_size + 1);
			strncpy(value, pos, num_size);
			value[num_size] = '\0';
			Token tok = {
				tok.type  = TOK_NUMBER,
				tok.value = value,
			};
			tokens[*num_tokens] = tok;
			(*num_tokens)++;
			tokenizer.offset += num_size;
			// token_print(&tok);
			continue;
		}
		// Check for binary operators
			size_t symbol_len = 0;
			if((symbol_len = tokenizer_is_symbol(pos, "+"))) {
				Token tok = tokenizer_make_symbol(a, TOK_PLUS, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "-"))) {
				Token tok = tokenizer_make_symbol(a, TOK_MINUS, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "*"))) {
				Token tok = tokenizer_make_symbol(a, TOK_MUL, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "/"))) {
				Token tok = tokenizer_make_symbol(a, TOK_DIV, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "^"))) {
				Token tok = tokenizer_make_symbol(a, TOK_CARET, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "@"))) {
				Token tok = tokenizer_make_symbol(a, TOK_AT, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};

		// Check for special symbols
		if(tokenizer_is_symbol(pos, "=") && tokenizer_lookahead(pos, "=")) {
			Token tok = tokenizer_make_symbol(a, TOK_DBL_EQ, &tokenizer, 2);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if(tokenizer_is_symbol(pos, "<") && tokenizer_lookahead(pos, "=")) {
			Token tok = tokenizer_make_symbol(a, TOK_LEQ, &tokenizer, 2);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if(tokenizer_is_symbol(pos, ">") && tokenizer_lookahead(pos, "=")) {
			Token tok = tokenizer_make_symbol(a, TOK_GEQ, &tokenizer, 2);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "<"))) {
			Token tok = tokenizer_make_symbol(a, TOK_LT, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, ">"))) {
			Token tok = tokenizer_make_symbol(a, TOK_GT, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "="))) {
			Token tok = tokenizer_make_symbol(a, TOK_EQ, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "("))) {
			Token tok = tokenizer_make_symbol(a, TOK_LPAREN, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, ")"))) {
			Token tok = tokenizer_make_symbol(a, TOK_RPAREN, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "["))) {
			Token tok = tokenizer_make_symbol(a, TOK_LBRACKET, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "]"))) {
			Token tok = tokenizer_make_symbol(a, TOK_RBRACKET, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "{"))) {
			Token tok = tokenizer_make_symbol(a, TOK_LCURLY, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "}"))) {
			Token tok = tokenizer_make_symbol(a, TOK_RCURLY, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, ","))) {
			Token tok = tokenizer_make_symbol(a, TOK_COMMA, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};

		fprintf(stderr,"Unexpected character at offset %zu: %c\n", tokenizer.offset, pos[0]);
		return 1;

	}

	Token eof = { .type = TOK_EOF };
	tokens[(*num_tokens)++] = eof;

	return 0;

}

//============================== 
//
//    Parsing
//
//============================== 

// X-Macros are convenient here :)

#define AST_NODES_LIST           \
	      X(AST_PROGRAM)           \
	      X(AST_STATEMENT_LIST)    \
	      X(AST_STATEMENT)         \
	      X(AST_LITERAL)        \
	      X(AST_ID)        \
	      X(AST_INDEXING)        \
	      X(AST_ARRAY)        \
	      X(AST_TUPLE)        \
	      X(AST_UNARY_OP)        \
	      X(AST_BINARY_OP)        \
	      X(AST_FN_CALL)        \
	      X(AST_ARGS_LIST)         \
	      X(AST_NODE_TYPE_COUNT)
				
typedef enum {
#define X(name) name,
	AST_NODES_LIST
#undef X
} ASTNodeType;


#define OP_TYPE_LIST         \
	      X(OP_PLUS)           \
	      X(OP_MINUS)          \
	      X(OP_MUL)            \
	      X(OP_DIV)            \
	      X(OP_POW)            \
	      X(OP_MMUL)           \
				X(OP_DBL_EQ)         \
				X(OP_GEQ)            \
				X(OP_LEQ)            \
				X(OP_LT)             \
				X(OP_GT)             \
	      X(OP_TYPE_LIST_COUNT)

typedef enum {
#define X(name) name,
	OP_TYPE_LIST
#undef X
} OpType;

void op_type_repr(OpType type, char *buf){
	int repr_size = 0;
	switch(type){
#define X(name) case name: { repr_size = sprintf(buf, #name); break;}
#define V(name) case name: { repr_size = sprintf(buf, #name); break;}
		OP_TYPE_LIST
#undef X
#undef V
	}
	buf[repr_size] = '\0';
}

OpType op_from_tok(TokenType type){
	switch (type) {
		case TOK_PLUS:    return OP_PLUS;
	  case TOK_MINUS:   return OP_MINUS;
	  case TOK_MUL:     return OP_MUL;
	  case TOK_DIV:     return OP_DIV;
	  case TOK_CARET:   return OP_POW;
	  case TOK_AT:      return OP_MMUL;
	  case TOK_DBL_EQ:  return OP_DBL_EQ;
	  case TOK_GEQ:     return OP_GEQ;
	  case TOK_LEQ:     return OP_LEQ;
	  case TOK_LT:      return OP_LT;
	  case TOK_GT:      return OP_GT;
		default:          UNREACHABLE("op_from_tok");
	}
}

// Expression value types

typedef enum {
	VALUE_ID,
	VALUE_NUMBER,
	VALUE_COMPLEX,
	VALUE_STRING,
	VALUE_POLYNOMIAL,
	VALUE_TRANSFER_FUNCTION,
	VALUE_NAN,

  // User-defined functions

	VALUE_FUNCTION,

	// Things to think about
	VALUE_ARRAY,
	VALUE_TUPLE,
	VALUE_MATRIX,
	VALUE_TYPE_COUNT
} ValueType;

typedef enum {
	STATEMENT_DECL_ASSIGN,
	STATEMENT_ASSIGN,
	STATEMENT_FUNCTION_DEF,
	STATEMENT_IF,
	STATEMENT_RETURN,
	STATEMENT_EXPR,
} StatementType;

// Forward declarations
typedef struct AST AST;

struct Value {
	ValueType valueType;
	union {
		Polynomial       poly;
		TransferFunction tf;
		double           number;
		double complex   complex_number;
		char             *str;
		char             *id;
		Array            array;
		Tuple            tuple;
    Function         fun;
	} value;
};


struct AST {
	ASTNodeType   type;
  StatementType statement_type;
	AST*          parent;
	AST*          next; // Optional, useful for linked lists

	union {
		struct {
			size_t num_statements;
			AST    *statement_list;
		} program;

		struct {
			char          *dst;
			AST           *expr;
		} statement;

		struct {
			size_t count;
			AST    *first;
		} statement_list;


		struct {
			AST *condition;
			AST *if_block;
			AST *else_block;
      int else_is_elif;
		} if_stmt;

		struct {
			char   *id;
			AST    *params;
			AST    *block;
      size_t num_args;
		} fn_def;

		Value literal;

		struct   {
			size_t size;
			AST    *first;
		} array;

		struct   {
			size_t size;
			AST    *first;
		} tuple;



		struct   {
			char   *fn_name;
			size_t num_arguments;
			AST    *first_arg;
		} fn_call;

		struct   {
			char   *var_name;
			AST    *index_expr;
		} indexing;

		struct {
			OpType op;
			AST    *left;
			AST    *right;
		} binop;

		struct {
			OpType op;
			AST    *arg;
		} unop;
	} as;
};

void ast_node_repr(AST *ast, char *buf){
	int repr_size = 0;
	switch(ast->type){
#define X(name) case name:   { repr_size = sprintf(buf, #name); break;}
		AST_NODES_LIST
#undef X
	}
	buf[repr_size] = '\0';
}

void ast_node_print(AST *ast){
	char buf[256];
	ast_node_repr(ast, buf);
	printf("%s\n", buf);
}

typedef struct {
	Token  *tokens;
	size_t num_tokens;
	size_t next;
} Parser;

Token *parser_peek(Parser *parser){
	if(parser->next < parser->num_tokens) return &parser->tokens[parser->next];
	return &parser->tokens[parser->num_tokens - 1];
}

int parser_done(Parser *parser){
	return parser_peek(parser)->type == TOK_EOF;
}


AST *ast_alloc(Arena *pa, ASTNodeType type){
	AST *node = arena_alloc(pa, sizeof(AST));
	memset(node, 0, sizeof(AST));
	node->type = type;
	return node;
}

Token *parser_eat(TokenType type, Parser *parser){
	if(parser_done(parser)) return NULL;
	Token *next = parser_peek(parser);
	if(next->type == type){
		parser->next++;
		return next;
	}
	return NULL;
}


Token *parser_keyword_eat(const char* expected, Parser *parser){
  Token *kw = parser_eat(TOK_KEYWORD, parser);
  if(kw == NULL) return NULL;
  if(strcmp(kw->value, expected) == 0){
    return kw;
  }
	return NULL;
}

Token *parser_keyword_match(const char* expected, Token *kw){
  if(kw->type != TOK_KEYWORD) return NULL;
  if(strcmp(kw->value, expected) == 0){
    return kw;
  }
	return NULL;
}



Token *parser_lookahead(Parser *parser){
	if(parser->next + 1 >= parser->num_tokens) return NULL;
	Token *la = &parser->tokens[parser->next + 1];
	return la;
}

int parser_eat_stmt_end(Parser *parser){
	if(parser_done(parser))                             return 1;
	if(parser_eat(TOK_NL, parser))                      return 1;
	if(parser->tokens[parser->next].type == TOK_RCURLY) return 1;
	return 0;
}



AST *ast_make_string(Arena *pa, size_t len){
	AST *expr = ast_alloc(pa, AST_LITERAL);
	expr->statement_type       = STATEMENT_EXPR;
	expr->as.literal.value.str = arena_alloc(pa, len + 1);
	expr->as.literal.valueType = VALUE_STRING;
	return expr;
}

AST *ast_make_number(Arena *pa, double number){
	AST *expr = ast_alloc(pa, AST_LITERAL);
	expr->statement_type          = STATEMENT_EXPR;
	expr->as.literal.value.number = number;
	expr->as.literal.valueType    = VALUE_NUMBER;
	return expr;
}

AST *ast_make_id(Arena *pa, size_t len){
	AST *expr = ast_alloc(pa, AST_ID);
	expr->statement_type       = STATEMENT_EXPR;
	expr->as.literal.value.id  = arena_alloc(pa, len + 1);
	expr->as.literal.valueType = VALUE_ID;
	return expr;
}

AST *ast_make_indexing(Arena *pa, size_t len){
	AST *expr = ast_alloc(pa, AST_INDEXING);
	expr->statement_type         = STATEMENT_EXPR;
	expr->as.indexing.var_name   = arena_alloc(pa, len + 1);
	expr->as.indexing.index_expr = NULL;
	return expr;
}


AST *ast_make_array(Arena *pa){
	AST *array = ast_alloc(pa, AST_ARRAY);
	array->statement_type = STATEMENT_EXPR;
	array->as.array.first = NULL;
	array->as.array.size  = 0;
	return array;
}

AST *ast_make_tuple(Arena *pa){
	AST *tuple = ast_alloc(pa, AST_TUPLE);
	tuple->statement_type = STATEMENT_EXPR;
	tuple->as.tuple.first = NULL;
	tuple->as.tuple.size  = 0;
	return tuple;
}

AST *ast_make_fn_call(Arena *pa, const char *fn_name){
	AST *fn_call = ast_alloc(pa, AST_FN_CALL);
	fn_call->statement_type           = STATEMENT_EXPR;
	fn_call->as.fn_call.num_arguments = 0;
	fn_call->as.fn_call.fn_name       = arena_alloc(pa, strlen(fn_name) + 1);
	fn_call->as.fn_call.first_arg     = NULL;
	strcpy(fn_call->as.fn_call.fn_name, fn_name);
	return fn_call;
}

AST *ast_make_unop(Arena *pa, OpType op){
	AST *unop = ast_alloc(pa, AST_UNARY_OP); 
	unop->statement_type = STATEMENT_EXPR;
	unop->as.unop.op     = op;
	unop->as.unop.arg    = NULL;
	return unop;
}


AST *ast_make_binop(Arena *pa, OpType op){
	AST *binop = ast_alloc(pa, AST_BINARY_OP); 
	binop->statement_type = STATEMENT_EXPR;
	binop->as.binop.left  = NULL;
	binop->as.binop.right = NULL;
	binop->as.binop.op    = op;
	return binop;
}

AST *ast_make_statement(Arena *pa, StatementType type){
	AST *stmt            = ast_alloc(pa, AST_STATEMENT);
	stmt->statement_type = type;
	return stmt;
}

AST *ast_make_statement_list(Arena *pa){
	AST *stmt_list                     = ast_alloc(pa, AST_STATEMENT_LIST);
	stmt_list->as.statement_list.count = 0;
	return stmt_list;
}



AST *parse_expression(Arena *pa, AST *parent, Parser *parser); // Forward declaration
AST *parse_term(Arena *pa, AST *parent, Parser *parser);
AST *parse_unary(Arena *pa, AST *parent, Parser *parser);
AST *parse_expo(Arena *pa, AST *parent, Parser *parser);
AST *parse_primary(Arena *pa, AST *parent, Parser *parser);
AST *parse_array(Arena *pa, AST *parent, Parser *parser);
AST *parse_statement_list(Arena *pa, AST *program_node, Parser *parser);


AST *parse_array(Arena *pa, AST *parent, Parser *parser){
	if(parser_eat(TOK_LBRACKET, parser) == NULL) return NULL;
	AST *array = ast_make_array(pa);
	if(!parser_done(parser) && parser_peek(parser)->type == TOK_RBRACKET){
		if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
		return array;
	}

	AST *head = parse_expression(pa, parent, parser);
	if(!head) return NULL;
	if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);

	array->as.array.first = head;
	AST *tail             = head;

	size_t size = 1;

	while(!parser_done(parser) && parser_peek(parser)->type != TOK_RBRACKET){
		AST *expr   = parse_expression(pa, parent, parser);
		if(!expr) return NULL;
		if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);
		tail->next  = expr;
		tail        = expr;
		size       += 1;
	}

	if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
	array->as.array.size = size;
	return array;
}

AST *parse_fn_call(Arena *pa, AST *parent, Parser *parser){
    Token *name = parser_eat(TOK_ID, parser);
    if(!name) return NULL;
    char *fn_name = name->value;
		if(parser_eat(TOK_LPAREN, parser) == NULL) return NULL;
		AST *fn_call = ast_make_fn_call(pa, fn_name);
		if(!parser_done(parser) && parser_peek(parser)->type == TOK_RPAREN){
			if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
			fn_call->as.fn_call.num_arguments = 0;
			return fn_call;
		}

		AST *head = parse_expression(pa, parent, parser);
		if(!head) return NULL;
		if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);
		AST *tail = NULL;
		if(head == NULL){
			if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
			fn_call->as.fn_call.num_arguments = 0;
			return fn_call;
		}
		fn_call->as.fn_call.first_arg = head;
		tail                          = head;
		size_t num_args               = 1;

		while(!parser_done(parser) && parser_peek(parser)->type != TOK_RPAREN){
			AST *expr  = parse_expression(pa, parent, parser);
			if(!expr) return NULL;
			if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);
			tail->next = expr;
			tail       = expr;
			num_args++;
		}

		if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
		fn_call->as.fn_call.num_arguments = num_args;
		return fn_call;
}

AST *parse_number(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
		if((current = parser_eat(TOK_NUMBER, parser)) == NULL) return NULL;
		AST *expr = ast_make_number(pa, atof(current->value));
		expr->parent = parent;
		return expr;
}

AST *parse_id(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
		if((current = parser_eat(TOK_ID, parser)) == NULL) return NULL;
    size_t len = strlen(current->value);
		AST *id    = ast_make_id(pa, len);
    strcpy(id->as.literal.value.id, current->value);
		id->parent = parent;
		return id;
}

AST *parse_indexing(Arena *pa, AST *parent, Parser *parser){
    Token *name = parser_eat(TOK_ID, parser);
    if(!name) return NULL;
    char *var_name = name->value;
		if(parser_eat(TOK_LBRACKET, parser) == NULL) return NULL;

		AST *indexing = ast_make_indexing(pa, strlen(var_name));
    strcpy(indexing->as.indexing.var_name, name->value);
		AST *expr  = parse_expression(pa, parent, parser);
		if(!expr) return NULL;
		indexing->as.indexing.index_expr = expr;

		if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
		return indexing;
}

AST *parse_string(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
		if((current = parser_eat(TOK_STRING, parser)) == NULL) return NULL;
    size_t len = strlen(current->value);
		AST *expr = ast_make_string(pa, len);
    strcpy(expr->as.literal.value.str, current->value);
		expr->parent = parent;
		return expr;
}


AST *parse_tuple_tail(Arena *pa, AST *parent, Parser *parser);
AST *parse_tuple_rest(Arena *pa, AST *parent, Parser *parser) {
    if(parser_eat(TOK_COMMA, parser) == NULL) return NULL;
    return parse_tuple_tail(pa, parent, parser);
}

AST *parse_tuple_tail(Arena *pa, AST *parent, Parser *parser) {
    size_t save = parser->next;
    AST *expr = NULL;
    if((expr = parse_expression(pa, parent, parser)) == NULL) {
        parser->next = save; // Restore cursor on epsilon!
        return NULL;
    }
    expr->next = parse_tuple_rest(pa, parent, parser);
    return expr;
}


AST *parse_tuple(Arena *pa, AST *parent, Parser *parser) {
    AST* head = NULL;
    if(parser_eat(TOK_LPAREN, parser) == NULL) return NULL;

    if((head = parse_expression(pa, parent, parser)) == NULL) return NULL;
    if(parser_eat(TOK_COMMA, parser) == NULL) return NULL;

    head->next = parse_tuple_tail(pa, parent, parser);

    if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
    AST *tuple = ast_make_tuple(pa);
    tuple->as.tuple.first = head;

    size_t size = 0;
    for(AST *node = head; node != NULL; node = node->next) { size++; }
    tuple->as.tuple.size = size;
    return tuple;
}

AST *parse_paren_expr(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
    if(parser_eat(TOK_LPAREN, parser) == NULL) return NULL;
    AST *expr = parse_expression(pa, parent, parser);
    if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
    return expr;
}

AST *parse_primary(Arena *pa, AST *parent, Parser *parser) {
	Token *current = parser_peek(parser);
  size_t save = parser->next;
  AST *primary = NULL;

  if(
      (parser->next = save, primary = parse_number(pa, parent, parser))     ||
      (parser->next = save, primary = parse_fn_call(pa, parent, parser))    ||
      (parser->next = save, primary = parse_indexing(pa, parent, parser))   ||
      (parser->next = save, primary = parse_id(pa, parent, parser))         ||
      (parser->next = save, primary = parse_string(pa, parent, parser))     ||
      (parser->next = save, primary = parse_array(pa, parent, parser))      ||
      (parser->next = save, primary = parse_tuple(pa, parent, parser))      ||
      (parser->next = save, primary = parse_paren_expr(pa, parent, parser))
    ){
    return primary;
  }
  return NULL;
}

AST *parse_term(Arena *pa, AST *parent, Parser *parser){
	AST *left = parse_unary(pa, parent, parser);
	if(!left) return NULL;
	while(!parser_done(parser) &&
			(parser_peek(parser)->type == TOK_MUL ||
			 parser_peek(parser)->type == TOK_AT  ||
			 parser_peek(parser)->type == TOK_DIV 
			 )
		)
	{
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_unary(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = left;
		binop->as.binop.right = right;
		left = binop;
	}
	return left;
}

AST *parse_unary(Arena *pa, AST *parent, Parser *parser){
  if(parser_peek(parser)->type == TOK_PLUS || parser_peek(parser)->type == TOK_MINUS){
    OpType op;

    if(parser_peek(parser)->type == TOK_PLUS)  op = OP_PLUS;
    else if(parser_peek(parser)->type == TOK_MINUS) op = OP_MINUS;
    else return NULL;

    parser->next++;

    AST *unop         = ast_make_unop(pa, op)  ;
    unop->as.unop.arg = parse_unary(pa, unop, parser);
		if(!unop->as.unop.arg) return NULL;

    return unop;
  }
  return parse_expo(pa, parent, parser);

}

AST *parse_expo(Arena *pa, AST *parent, Parser *parser){
	AST *primary = parse_primary(pa, parent, parser);
	if(!primary) return NULL;
	if(!parser_done(parser) && parser_peek(parser)->type == TOK_CARET){
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_unary(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = primary;
		binop->as.binop.right = right;
		return binop;
	}
	return primary;
}

AST *parse_additive(Arena *pa, AST *parent, Parser *parser){
	AST *left = parse_term(pa, parent, parser);
	if(!left) return NULL;
	while(!parser_done(parser) && (
			parser_peek(parser)->type == TOK_PLUS ||
			parser_peek(parser)->type == TOK_MINUS)
		)
	{
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_term(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = left;
		binop->as.binop.right = right;
		left = binop;
	}
	return left;
}

AST *parse_expression(Arena *pa, AST *parent, Parser *parser){
	AST *left = parse_additive(pa, parent, parser);
	if(!left) return NULL;
	while(!parser_done(parser) && (
			parser_peek(parser)->type == TOK_DBL_EQ ||
			parser_peek(parser)->type == TOK_LT     ||
			parser_peek(parser)->type == TOK_GT     ||
			parser_peek(parser)->type == TOK_LEQ    ||
			parser_peek(parser)->type == TOK_GEQ)
		)
	{
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_additive(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = left;
		binop->as.binop.right = right;
		left = binop;
	}
	return left;
}


void parse_params(Arena *pa, AST *fn_def, Parser *parser){
	fn_def->as.fn_def.num_args = 0;
	if(parser_peek(parser)->type != TOK_ID) return;

	AST *tail = NULL;

	while(parser_peek(parser)->type == TOK_ID){
		Token *id_tok = parser_eat(TOK_ID, parser);
		if(id_tok == NULL) return;

		AST *node  = ast_make_id(pa, strlen(id_tok->value));
    strcpy(node->as.literal.value.id, id_tok->value);
		node->next = NULL;

		if(tail == NULL){
			fn_def->as.fn_def.params = node;
		}else {
			tail->next = node;
		}

		tail = node;
		fn_def->as.fn_def.num_args++;

		if(parser_peek(parser)->type != TOK_COMMA){
			return;
		} else parser_eat(TOK_COMMA, parser);
	}

}

AST *parse_block(Arena *pa, AST *parent, Parser *parser){
  if(parser_eat(TOK_LCURLY, parser) == NULL) return NULL;
  AST *list = parse_statement_list(pa, parent, parser);
	if(!list) return NULL;
  if(parser_eat(TOK_RCURLY, parser) == NULL) return NULL;
  return list;
}

AST *parse_assignment(Arena *pa, AST *parent, Parser *parser){
	size_t save    = parser->next;
  Token *current = NULL;
  AST   *stmt    = NULL;
  if((current = parser_peek(parser)) && 
      parser_keyword_eat("let", parser))
  {
    stmt = ast_make_statement(pa, STATEMENT_DECL_ASSIGN);
  }else{
		parser->next = save;
    stmt = ast_make_statement(pa, STATEMENT_ASSIGN);
  }

  Token *id = NULL;
  if((id = parser_eat(TOK_ID, parser)) == NULL) return NULL;

  stmt->as.statement.dst  = arena_alloc(pa, strlen(id->value) + 1);
  strcpy(stmt->as.statement.dst, id->value);

  if((id = parser_eat(TOK_EQ, parser)) == NULL) return NULL;

  AST *expr               = parse_expression(pa, stmt, parser);
	if(!expr) return NULL;
  stmt->as.statement.expr = expr;

  if(!parser_eat_stmt_end(parser)) return NULL;
  return stmt;
}

AST *parse_if_stmt(Arena *pa, AST *parent, Parser *parser){
    if(parser_keyword_eat("if", parser)) {
      AST *stmt                     = ast_make_statement(pa, STATEMENT_IF);
      AST *condition                = parse_expression(pa, stmt, parser);
      AST *if_block                 = parse_block(pa, stmt, parser);

			if(!condition) return NULL;
			if(!if_block)  return NULL;

      stmt->as.if_stmt.condition    = condition;
      stmt->as.if_stmt.if_block     = if_block;
      stmt->as.if_stmt.else_is_elif = 0;

      AST *current = stmt;
      Token *tok   = NULL;

      if((tok = parser_peek(parser)) && 
          parser_keyword_match("elif", tok))
      {
        parser_eat(TOK_KEYWORD, parser);

        AST *elif_stmt                   = ast_make_statement(pa, STATEMENT_IF);
        elif_stmt->parent                = current;

				AST *elif_condition              = parse_expression(pa, elif_stmt, parser);
				AST *elif_block                  = parse_block(pa, elif_stmt, parser);

				if(!elif_condition) return NULL;
				if(!elif_block)     return NULL;

        elif_stmt->as.if_stmt.condition  = elif_condition;
        elif_stmt->as.if_stmt.if_block   = elif_block;

        current->as.if_stmt.else_block   = elif_stmt;
        current->as.if_stmt.else_is_elif = 1;
        current                          = elif_stmt;

      }

			if((tok = parser_peek(parser)) && 
					parser_keyword_match("else", tok))
			{
        parser_eat(TOK_KEYWORD, parser);
				AST *else_block                  = parse_block(pa, stmt, parser);

				if(!else_block) return NULL;

        current->as.if_stmt.else_block   = else_block;
        current->as.if_stmt.else_is_elif = 0;
      }

			if(!parser_eat_stmt_end(parser)) return NULL;
      return stmt;
    }
    return NULL;
}

AST *parse_return_stmt(Arena *pa, AST *parent, Parser *parser){
    if(parser_keyword_eat("return", parser)) {
			size_t save             = parser->next;
      AST *stmt               = ast_make_statement(pa, STATEMENT_RETURN);
      AST *expr               = parse_expression(pa, stmt, parser);
			if(!expr) parser->next  = save;
			stmt->as.statement.expr = expr;
			if(!parser_eat_stmt_end(parser)) return NULL;
      return stmt;
    }
    return NULL;
}

AST *parse_fn_def(Arena *pa, AST *parent, Parser *parser){
  if(parser_keyword_eat("fn", parser) == NULL) return NULL;
  Token *id = NULL;

  AST *stmt = ast_make_statement(pa, STATEMENT_FUNCTION_DEF);
  if((id = parser_eat(TOK_ID, parser)) == NULL) return NULL;

  if(!parser_eat(TOK_LPAREN, parser)) return NULL;

  size_t len              = strlen(id->value);
  stmt->as.fn_def.id      = arena_alloc(pa, len + 1);
  strcpy(stmt->as.fn_def.id, id->value);

  stmt->as.fn_def.id[len] = '\0';
  parse_params(pa, stmt, parser);

  if(!parser_eat(TOK_RPAREN, parser)) return NULL;

	AST *block = parse_block(pa, stmt, parser);
	if(!block) return NULL;
  stmt->as.fn_def.block = block;

	if(!parser_eat_stmt_end(parser)) return NULL;
  return stmt;
}

AST *parse_expr_stmt(Arena *pa, AST *parent, Parser *parser){
  AST *expr = parse_expression(pa, parent, parser);
  if(expr == NULL) return NULL;
	if(!parser_eat_stmt_end(parser)) return NULL;
  return expr;
}

AST *parse_statement(Arena *pa, AST *parent, Parser *parser){
  size_t save = parser->next;
	Token *current = parser_peek(parser);
  AST *stmt = NULL;

  if(
      (parser->next = save, (stmt = parse_assignment(pa, parent, parser))) ||
      (parser->next = save, (stmt = parse_return_stmt(pa, parent, parser)))    ||
      (parser->next = save, (stmt = parse_if_stmt(pa, parent, parser)))    ||
      (parser->next = save, (stmt = parse_fn_def(pa, parent, parser)))     ||
      (parser->next = save, (stmt = parse_expr_stmt(pa, parent, parser)))
    ) return stmt;

  return NULL;
}

AST *parse_statement_list(Arena *pa, AST *parent, Parser *parser){
	AST *stmt_list = ast_make_statement_list(pa);
	AST *tail = NULL;

  while(!parser_done(parser))
  {

    while(!parser_done(parser) && parser_peek(parser)->type == TOK_NL) {
      parser_eat(TOK_NL, parser);
    }

    if(parser_done(parser) || parser_peek(parser)->type == TOK_RCURLY) {
      break;
    }

    size_t before = parser->next;

    AST *stmt = parse_statement(pa, parent, parser);

    if (stmt == NULL || parser->next == before) {
      return NULL;
    }

    stmt->parent = parent;

    if (tail) tail->next = stmt;
    else stmt_list->as.statement_list.first = stmt;
		stmt_list->as.statement_list.count++;

    tail = stmt;
  }

	return stmt_list;
}



int parse_tokens(Arena *pa, AST *ast, Token *tokens, size_t num_tokens){
	ast->parent = NULL;
	ast->type = AST_PROGRAM;

	Parser parser;
	parser.num_tokens = num_tokens;
	parser.tokens     = tokens;
	parser.next       = 0;

	AST *list = parse_statement_list(pa, ast, &parser);
	ast->as.program.statement_list = list;

	if(!parser_done(&parser)) return 1;

	return list == NULL ? 1 : 0;
}

void fill_indent(size_t level, char *buf){
	size_t indent_size = 2;
	size_t total_spaces = indent_size * level;
	for(size_t i = 0; i < total_spaces; i++){
		buf[i] = ' ';
	}
	buf[total_spaces] = '\0';
}

void ast_print(AST *ast, int level){
	char buf[256];
	fill_indent(level, buf);
	switch(ast->type){
		case AST_PROGRAM:
			{
				printf("%sPROGRAM:\n", buf);
				AST *stmt = ast->as.program.statement_list;
				while(stmt != NULL){
					ast_print(stmt, level + 1);
					stmt = stmt->next;
				}
				break;
			}
		case AST_STATEMENT_LIST:
			{
				printf("%sSTMT_LIST:\n", buf);
        for(
            AST* node = ast->as.statement_list.first;
            node != NULL;
            node = node->next
          )
        {
					ast_print(node, level + 1);
        }
				break;
			}
		case AST_STATEMENT:
			{
				printf("%sSTMT:\n", buf);
				fill_indent(level + 1, buf);
				printf("%sSRC:  %s\n", buf, ast->as.statement.dst);
				printf("%sEXP:\n", buf);
				ast_print(ast->as.statement.expr, level + 2);
				break;
			}
		case AST_ID:
			{
        printf("%sID: %s\n", buf, ast->as.literal.value.id);
				break;
			}
		case AST_LITERAL:
			{
				if(ast->as.literal.valueType == VALUE_STRING){
					printf("%sLIT: %s\n", buf, ast->as.literal.value.str);
				} else if(ast->as.literal.valueType == VALUE_NUMBER){
					printf("%sLIT: %.2f\n", buf, ast->as.literal.value.number);
				}
				break;
			}
		case AST_TUPLE:
			{
				printf("%sTUPLE[%zu]:\n", buf, ast->as.tuple.size);
        for(
            AST* node = ast->as.tuple.first;
            node != NULL;
            node = node->next
          )
        {
					ast_print(node, level + 1);
        }
				break;
			}
		case AST_INDEXING:
			{
				printf("%sINDEXING[%s]:\n",
						buf,
						ast->as.indexing.var_name);
				ast_print(ast->as.indexing.index_expr, level + 1);
				break;
			}
		case AST_FN_CALL:
			{
				printf("%sFN[%s]:\n", buf, ast->as.fn_call.fn_name);
				AST *arg = ast->as.fn_call.first_arg;
				while(arg != NULL){
					ast_print(arg, level + 1);
					arg = arg->next;
				}
				break;
			}
		case AST_BINARY_OP:
			{
				char opBuf[128];
				op_type_repr(ast->as.binop.op, opBuf);
				printf("%sBINOP[%s]:\n", buf, opBuf);
				fill_indent(level + 1, buf);
				printf("%sLEFT:\n", buf);
				ast_print(ast->as.binop.left, level + 2);
				fill_indent(level + 1, buf);
				printf("%sRIGHT:\n", buf);
				ast_print(ast->as.binop.right, level + 2);
				break;
			}
	}
}

//============================== 
//
//    Environment
//
//============================== 

typedef struct {
	char  *name;
	Value *value;
} Variable;

typedef struct Environment Environment;
struct Environment {
  Arena       *persistent;
  Arena       *frame;
	Variable    *vars;
	size_t      num_vars;
	Environment *parent;
	size_t      *failed_asserts;
	char        *exe_path;
};

void initialize_native_fns(Environment *e);
void env_init(Environment *e, Arena *pa){
  e->persistent = pa;
	e->vars       = arena_alloc(pa, 32 * sizeof(Variable));
	e->num_vars   = 0;
	e->parent     = NULL;
}


Environment env_create(Arena *pa, Arena *fa, size_t *failed_asserts, char* exe_path){
	Environment e;
	env_init(&e, pa);
  e.frame          = fa;
	e.failed_asserts = failed_asserts;
	e.exe_path       = exe_path;
  initialize_native_fns(&e);
	return e;
}

Arena *env_storage(Environment *e) {
  return e->parent ? e->frame : e->persistent;
}

int env_variable_idx(Environment *e, const char* name){
	for(int i = 0; i < e->num_vars; i++){
		if(strcmp(name, e->vars[i].name) == 0) return i;
	}
	return -1;
}

Variable *env_get_variable_from_idx(Environment *e, int idx){
	if(idx < 0) return NULL;
	if(idx >= e->num_vars) return NULL;
	return &e->vars[idx];
}

Variable *env_find_local(Environment *e, const char* name){
	if(e == NULL) return NULL;
	int i = env_variable_idx(e, name);
	if(i >= 0) return env_get_variable_from_idx(e, i);
	return NULL;

}
Variable *env_find_variable(Environment *e, const char* name){
	if(e == NULL) return NULL;

	for(
			Environment *env  = e;
			env              != NULL;
			env               = env->parent
	){
		int i = env_variable_idx( env, name);
		if(i >= 0) return env_get_variable_from_idx(env, i);
	}

	return NULL;

}



void env_alloc_variable(Environment *e, const char* name, Value *value){
  Arena *storage  = env_storage(e);

	Variable *var   = &e->vars[e->num_vars];
	size_t name_len = strlen(name);

  var->name       = arena_alloc(storage, name_len + 1);
	strcpy(var->name, name);

  var->value      = arena_alloc(storage, sizeof(Value));
	*var->value     = *value;

	e->num_vars++;

	// TODO: Implement reallocation
}

void env_set_variable(Environment *e, const char* name, Value *value){
	int idx =  env_variable_idx(e, name);
	if(idx < 0){
		env_alloc_variable(e, name, value);
		return;
	}

	Variable *var       = &e->vars[idx];
	*var->value         = *value;
}

void  env_list_variables(Environment  *e){
	for(size_t i = 0; i < e->num_vars; i++){
		printf("%s\n", e->vars[i].name);
	}
}

//============================== 
//
//    Interpreting
//
//============================== 


Value make_num(double num){
	Value v = {
		.valueType    = VALUE_NUMBER,
		.value.number = num
	};
	return v;
}
Value make_complex(double complex num){
	Value v = {
		.valueType            = VALUE_COMPLEX,
		.value.complex_number = num
	};
	return v;
}
Value make_array(Array arr){
	Value v = {
		.valueType   = VALUE_ARRAY,
		.value.array = arr
	};
	return v;
}

Value make_function(Function fun){
	Value v = {
		.valueType = VALUE_FUNCTION,
		.value.fun = fun
	};
	return v;
}

Value value_clone(Arena *a, Value *v);

Array clone_array(Arena *a, Array *old){
  Array arr;
  arr.size = old->size;
  arr.nums = arena_alloc(a, arr.size * sizeof(double));
  memcpy(arr.nums, old->nums, arr.size * sizeof(double));
  return arr;
}

Tuple clone_tuple(Arena *a, Tuple *src){
  Tuple dst;

  dst.size  = src->size;
  dst.items = arena_alloc(a, src->size * sizeof(Value));

  for(size_t i = 0; i < src->size; i++){
    dst.items[i] = value_clone(a, &src->items[i]);
  }
  return dst;
}

Polynomial clone_poly(Arena *a, Polynomial *p){
  Polynomial poly;
  poly.num_coeffs = p->num_coeffs;
  poly.deg        = p->deg;
  poly.coeffs     = arena_alloc(a, poly.num_coeffs * sizeof(double));
  memcpy(poly.coeffs, p->coeffs, poly.num_coeffs * sizeof(double));
  return poly;
}

TransferFunction clone_tf(Arena *a, TransferFunction *tf){
	Polynomial num = clone_poly(a, &tf->num);
	Polynomial den = clone_poly(a, &tf->den);
	TransferFunction tf_new = {
		.num = num,
		.den = den
	};
  return tf_new;
}

char *clone_string(Arena *a, char *old){
  size_t len = strlen(old);
  char *new = arena_alloc(a, len + 1);
  strcpy(new, old);
  return new;
}

Value value_clone(Arena *a, Value *v){
  Value copy = *v;
  switch(v->valueType){
    case VALUE_COMPLEX: 
      {
        copy.value.complex_number = v->value.complex_number;
        break;
      }
    case VALUE_NUMBER: 
      {
        copy.value.number = v->value.number;
        break;
      }
    case VALUE_ID: 
      {
        copy.value.id = clone_string(a, v->value.id);
        break;
      }
    case VALUE_STRING: 
      {
        copy.value.str = clone_string(a, v->value.str);
        break;
      }
    case VALUE_ARRAY: 
      {
        copy.value.array = clone_array(a, &v->value.array);
        break;
      }
    case VALUE_TUPLE: 
      {
        copy.value.tuple = clone_tuple(a, &v->value.tuple);
        break;
      }
    case VALUE_POLYNOMIAL: 
      {
        copy.value.poly = clone_poly(a, &v->value.poly);
        break;
      }
    case VALUE_TRANSFER_FUNCTION: 
      {
				copy.value.tf = clone_tf(a, &v->value.tf);
        break;
      }
    default: break;
  }
  return copy;
}

Polynomial poly_new(Arena *a, double *src_coeffs, size_t num_coeffs){
	size_t deg = num_coeffs - 1;
	double *coeffs = arena_alloc(a, num_coeffs * sizeof(double));
  memcpy(coeffs, src_coeffs, num_coeffs * sizeof(double));
	Polynomial p = {
		.coeffs     = coeffs,
		.deg        = deg,
		.num_coeffs = num_coeffs
	};
	return p;
}

Value make_poly(Polynomial poly){
	Value v = {
		.valueType  = VALUE_POLYNOMIAL,
		.value.poly = poly
	};
	return v;
}

Value make_tuple(Tuple tuple){
	Value v = {
		.valueType   = VALUE_TUPLE,
		.value.tuple = tuple
	};
	return v;
}


Value make_transfer_function(TransferFunction tf){
	Value v = {
		.valueType = VALUE_TRANSFER_FUNCTION,
		.value.tf  = tf
	};
	return v;
}

Value make_string(char *str){
	Value v = {
		.valueType = VALUE_STRING,
		.value.str = str
	};
	return v;
}



Value make_nan(){
	Value v = {
		.valueType    = VALUE_NAN,
		.value.number = 0
	};
	return v;
}

//============================
//
//   Transformations
//
//============================

Polynomial poly_from_num(Arena *a, double n){
	Polynomial poly = alloc_poly(a, 0);
	poly.coeffs[0] = n;
	return poly;
}

Array scale_array(Arena *a, Array *arr, double x){
  Array arr_new = clone_array(a, arr);
	for(size_t i = 0; i < arr_new.size; i++){
		arr_new.nums[i] *= x;
	}
	return arr_new;
}

Polynomial scale_poly(Arena *a, Polynomial *p, double x){
	Polynomial poly = alloc_poly(a, p->deg);
	for(size_t i = 0; i < poly.num_coeffs; i++){
		poly.coeffs[i] = p->coeffs[i] * x;
	}
	return poly;
}


TransferFunction tf_invert(Arena *a, TransferFunction *tf){
	TransferFunction tf_new = {
		.num = clone_poly(a, &tf->den),
		.den = clone_poly(a, &tf->num),
	};
	return tf_new;
}

TransferFunction scale_tf(Arena *a, TransferFunction *tf, double x){
	TransferFunction new_tf = clone_tf(a, tf);
	new_tf.num = scale_poly(a, &new_tf.num, x);
	return new_tf;
}

TransferFunction tf_from_poly(Arena *a, Polynomial *p){
	Polynomial num = alloc_poly(a, p->deg);
	memcpy(num.coeffs, p->coeffs, p->num_coeffs * sizeof(double));
	Polynomial den = alloc_poly(a, 0);
	den.coeffs[0] = 1;
	TransferFunction tf = {
		.num = num,
		.den = den,
	};
	return tf;
}

TransferFunction tf_from_num(Arena *a, double n){
	Polynomial num_poly = poly_from_num(a, n);
	return tf_from_poly(a, &num_poly);
}

char *cat_strings(Arena *a, char *str1, char *str2){
  size_t len1    = strlen(str1);
  size_t len2    = strlen(str2);
  size_t new_len = len1 + len2;
  char *new_str  = alloc_string(a, new_len);

  for(size_t i = 0; i < len1; i++){
    new_str[i] = str1[i];
  }
  for(size_t i = len1; i < new_len; i++){
    new_str[i] = str2[i - len1];
  }
  new_str[new_len] = '\0';

  return new_str;
}

char *string_from_num(Arena *a, double num){
  char *num_str = arena_alloc(a, 1024);
  sprintf(num_str, "%f", num);
  return num_str;
}

//============================
//
//   Algorithms
//
//============================

int modulo(int a, int b){
	int r = a % b;
	return r < 0 ? r + b : r;
}

int poly_is_zero(Polynomial *p){
  double max = -1;
  for(size_t i = 0; i < p->num_coeffs; i++){
    max = MAX(max, fabs(p->coeffs[i]));
  }
  if(max < EPSILON){
    return 1;
  }
  return 0;
}

void poly_update_degree(Polynomial *p){
  int i = p->num_coeffs - 1;
  while(ISZERO(p->coeffs[i]) && i > 0){
		p->coeffs[i] = 0.0;
		i--;
	}
  p->num_coeffs = i + 1;
  p->deg        = i;
}

Polynomial poly_monic(Arena *a, Polynomial p){
  return scale_poly(a, &p, 1.0/p.coeffs[p.num_coeffs - 1]);
}

int poly_divmod(Arena *arena, Polynomial a, Polynomial b, Polynomial *q_out, Polynomial *r_out){
	poly_update_degree(&a);
	poly_update_degree(&b);

	if(poly_is_zero(&b)){
		printf("Polynomial division by zero.\n");
		return 1;
	}

	if(a.deg < b.deg || poly_is_zero(&a)){
		if(q_out) *q_out = poly_from_num(arena, 0.0);
		if(r_out) *r_out = clone_poly(arena, &a);
		return 0;
	}

	Polynomial q      = alloc_poly(arena, a.deg - b.deg);
	Polynomial r      = clone_poly(arena, &a);
	double lead_coeff = b.coeffs[b.num_coeffs - 1];

	while(r.deg >= b.deg && !poly_is_zero(&r)){
		size_t q_deg    = r.deg - b.deg;
		double coef     = r.coeffs[r.deg] / lead_coeff;
		q.coeffs[q_deg] = coef;

		for(int j = 0; j < b.num_coeffs; j++){
			r.coeffs[j + q_deg] -= coef * b.coeffs[j];
		}

		// To avoid floating point issues in the subtraction:
		r.coeffs[r.deg] = 0.0;
		poly_update_degree(&r);
	}

	if(q_out) *q_out = q;
	if(r_out) *r_out = r;

	return 0;
}

double poly_horner(Polynomial p, double x){

  // P(x) = a0 + a1*x + a2*x^2 + ... + an*x^n
  // P(x) = a0 + x * (a1 + x * (a2 + x * (a3) ))

  if(p.deg == 0) return p.coeffs[0];

  double val = p.coeffs[p.deg];
  for (int i = (int)p.deg - 1; i >= 0; i--) {
    val = val * x + p.coeffs[i];
  }

  return val;

}

void poly_horner_with_derivative(
    Polynomial p,
    double complex x,
    double complex *val_out,
    double complex *der_out
    )
{

  // P(x)  = a0 + a1*x + a2*x^2 + ... + an*x^n
  // P(x)  = a0 + x * (a1 + x * (a2 + x * (a3) ))
  // P'(x) = a1 + 2a2*x + 3a3*x^2 + 4a4x^3 + ... + n * an*x^n - 1
  // P'(x) = a1 + x * (2a2 + x * (3a3 + x * (4a4)))

  if(p.deg == 0) { 
    if(val_out) *val_out = p.coeffs[0];
    if(der_out) *der_out = 0;
    return;
  }

  double complex val = p.coeffs[p.deg];
  double complex der = 0.0;

  for (int i = (int)p.deg - 1; i >= 0; i--) {
    der = der * x + val;
    val = val * x + p.coeffs[i];
  }

  if(val_out) *val_out = val;
  if(der_out) *der_out = der;

}


void poly_aberth_solve(Arena *ta, Polynomial poly, double complex *roots) {
    double *coeffs = poly.coeffs;
    size_t deg = poly.deg;

    if (deg == 0 || coeffs == NULL || roots == NULL ||
        coeffs[deg] == 0.0) {
        return;
    }

    // Cauchy radius
    double radius = 0.0;

    for (size_t i = 0; i < deg; i++) {
        double ratio = fabs(coeffs[i] / coeffs[deg]);
        if (ratio > radius) radius = ratio;
    }

    // Initial estimates on a circle
    double M = radius + 1.0;
    double pi = acos(-1.0);
		double offset = 0.1;

    for (size_t i = 0; i < deg; i++) {
        double theta = 2.0 * pi * i / deg + offset;
        roots[i] = M * cexp(I * theta);
    }

    // Scratch memory
    size_t scratch = arena_save(ta);
    double complex *temp = arena_alloc(
        ta, deg * sizeof(double complex)
    );

    // Aberth-Ehrlich iterations
    bool converged = false;

    for (size_t iter = 0; iter < ABERTH_MAX_ITER; iter++) {
        double max_step = 0.0;

        for (size_t i = 0; i < deg; i++) {
            double complex zi = roots[i];
            double complex pz, der;
            double complex sum = 0.0;

            poly_horner_with_derivative(poly, zi, &pz, &der);

            if (pz == 0.0) {
                temp[i] = 0.0;
                continue;
            }

            // Aberth correction
            for (size_t j = 0; j < deg; j++) {
                if (i == j) continue;

                double complex diff = zi - roots[j];

                if (diff == 0.0) {
                    goto cleanup;
                }

                sum += 1.0 / diff;
            }

            double complex denom = der - pz * sum;

            if (denom == 0.0) {
                goto cleanup;
            }

            double complex delta = pz / denom;

            if (!isfinite(creal(delta)) ||
                !isfinite(cimag(delta))) {
                goto cleanup;
            }

            temp[i] = -delta;

            double step = cabs(delta) / (1.0 + cabs(zi));
            if (step > max_step) max_step = step;
        }

        // Simultaneous update
        for (size_t i = 0; i < deg; i++) {
            roots[i] += temp[i];
        }

        // Convergence
        if (max_step < ABERTH_TOL) {
            converged = true;
            break;
        }
    }

cleanup:
    if (!converged) {
        fprintf(stderr, "Aberth solver did not converge\n");
    }

    arena_restore(ta, scratch);
}




Polynomial poly_gcd(Arena *arena, Polynomial a, Polynomial b){
	// Euclid'a gcd algorithm for polynomials
	poly_update_degree(&a);
	poly_update_degree(&b);
	Polynomial q;
	Polynomial r;
  while (!poly_is_zero(&b)) {
    poly_divmod(arena, a, b, &q, &r);
    a = b;
    b = r;
  }
	return poly_monic(arena, a);
}

TransferFunction tf_simplify(Arena *a, TransferFunction *tf){
	Polynomial g = poly_gcd(a, tf->num, tf->den);

	Polynomial reduced_num = tf->num;
	Polynomial reduced_den = tf->den;
	poly_divmod(a, tf->num, g, &reduced_num, NULL);
	poly_divmod(a, tf->den, g, &reduced_den, NULL);

	TransferFunction new_tf = {
		.num = clone_poly(a, &reduced_num),
		.den = clone_poly(a, &reduced_den),
	};

	return new_tf;
}

double complex *tf_poles(Arena *ta, TransferFunction tf){
	Polynomial den = tf.den;
	double complex *roots = arena_alloc(ta, den.deg * sizeof(double complex));
	poly_aberth_solve(ta, den, roots);
	return roots;
}

//============================
//
//   Dispatcher types
//
//============================

typedef Value (*NativeValueDispatch)(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent);
typedef Value (*UnaryValueDispatch)(Arena *a, Value *caller, Value *value);

//============================
//
//   Unary Operations
//
//============================

typedef Value (*UnaryFn)(Arena *a, Value *arg);

Value unop_with_nan(Arena *a, Value *arg){
	return make_nan();
}

Value unop_num_plus(Arena *a, Value *arg){
	return make_num(arg->value.number);
}
Value unop_complex_plus(Arena *a, Value *arg){
	return make_complex(arg->value.complex_number);
}
Value unop_arr_plus(Arena *a, Value *arg){
	return make_array(arg->value.array);
}
Value unop_poly_plus(Arena *a, Value *arg){
	return make_poly(arg->value.poly);
}
Value unop_tf_plus(Arena *a, Value *arg){
	return make_transfer_function(arg->value.tf);
}

Value unop_num_minus(Arena *a, Value *arg){
	return make_num(-1.0 * arg->value.number);
}
Value unop_complex_minus(Arena *a, Value *arg){
	return make_complex(-1.0 * arg->value.complex_number);
}
Value unop_arr_minus(Arena *a, Value *arg){
  Array arr = scale_array(a, &arg->value.array, -1.0);
	return make_array(arr);
}
Value unop_poly_minus(Arena *a, Value *arg){
	return make_poly(scale_poly(a, &arg->value.poly, -1.0));
}
Value unop_tf_minus(Arena *a, Value *arg){
	return make_transfer_function(scale_tf(a, &arg->value.tf, -1.0));
}



static const UnaryFn unop_dispatch[OP_TYPE_LIST_COUNT][VALUE_TYPE_COUNT] = {
	[OP_PLUS] = {
		[VALUE_NUMBER]            = unop_num_plus,
		[VALUE_COMPLEX]           = unop_complex_plus,
		[VALUE_ARRAY]             = unop_arr_plus,
		[VALUE_POLYNOMIAL]        = unop_poly_plus,
    [VALUE_TRANSFER_FUNCTION] = unop_tf_plus,
		[VALUE_NAN]               = unop_with_nan,
    [VALUE_STRING]            = unop_with_nan,
	},

	[OP_MINUS] = {
		[VALUE_NUMBER]            = unop_num_minus,
		[VALUE_COMPLEX]           = unop_complex_minus,
		[VALUE_ARRAY]             = unop_arr_minus,
		[VALUE_POLYNOMIAL]        = unop_poly_minus,
    [VALUE_TRANSFER_FUNCTION] = unop_tf_minus,
		[VALUE_NAN]               = unop_with_nan,
    [VALUE_STRING]            = unop_with_nan,
	},

};

//============================
//
//   Binary Operations
//
//============================


typedef Value (*BinaryFn)(Arena *a, Value *left, Value *right);



Value op_with_nan(Arena *a, Value *left, Value *right){
	return make_nan();
}


double complex as_complex(Value *v){
	return v->valueType == VALUE_COMPLEX ? v->value.complex_number : v->value.number;
}

Value add_scalar_scalar(Arena *a, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number + right->value.number);
	return make_complex(as_complex(left) + as_complex(right));
}
Value sub_scalar_scalar(Arena *a, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number - right->value.number);
	return make_complex(as_complex(left) - as_complex(right));
}
Value mul_scalar_scalar(Arena *a, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number * right->value.number);
	return make_complex(as_complex(left) * as_complex(right));
}
Value div_scalar_scalar(Arena *a, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number / right->value.number);
	return make_complex(as_complex(left) / as_complex(right));
}
Value pow_scalar_scalar(Arena *a, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(pow(left->value.number , right->value.number));
	return make_complex(cpow(as_complex(left),  as_complex(right)));
}

// Real-specific

Value eq_scalar_scalar(Arena *a, Value *left, Value *right){
	return make_num(as_complex(left) == as_complex(right));
}
Value lt_num_num(Arena *a, Value *left, Value *right){
	return make_num(left->value.number < right->value.number);
}
Value gt_num_num(Arena *a, Value *left, Value *right){
	return make_num(left->value.number > right->value.number);
}
Value leq_num_num(Arena *a, Value *left, Value *right){
	return make_num(left->value.number <= right->value.number);
}
Value geq_num_num(Arena *a, Value *left, Value *right){
	return make_num(left->value.number >= right->value.number);
}

// ------------- Arrays --------------------

Value add_num_array(Arena *a, Value *left, Value *right){
		double num = left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;
		Array  arr = left->valueType == VALUE_ARRAY ? left->value.array : right->value.array;

    Array new_arr = clone_array(a, &arr);

		for(size_t i = 0; i < arr.size; i++){
			new_arr.nums[i] += num;
		}
		return make_array(new_arr);
}

Value add_array_array(Arena *a, Value *left, Value *right){
		Array arr1 = left->value.array;
		Array arr2 = right->value.array;
		if(arr1.size != arr2.size){
			printf("Incompatible array sizes: %zu and %zu\n", arr1.size, arr2.size);
			return make_nan();
		}
    Array new_arr = clone_array(a, &arr1);
		for(size_t i = 0; i < arr1.size; i++){
			new_arr.nums[i] = arr1.nums[i] + arr2.nums[i];
		}
		return make_array(new_arr);
}
Value sub_num_array(Arena *a, Value *left, Value *right){
		double num         = left->value.number;
		Array  arr         = right->value.array;

    Array new_arr      = clone_array(a, &arr);
		for(size_t i = 0; i < arr.size; i++){
			new_arr.nums[i] = num - arr.nums[i];
		}
		return make_array(new_arr);
}
Value sub_array_num(Arena *a, Value *left, Value *right){
		Array  arr         = left->value.array;
		double num         = right->value.number;

    Array new_arr      = clone_array(a, &arr);
		for(size_t i = 0; i < arr.size; i++){
			new_arr.nums[i] = arr.nums[i] - num;
		}
		return make_array(new_arr);
}
Value sub_array_array(Arena *a, Value *left, Value *right){
		Array arr1 = left->value.array;
		Array arr2 = right->value.array;
		if(arr1.size != arr2.size){
			printf("Incompatible array sizes: %zu and %zu\n", arr1.size, arr2.size);
			return make_nan();
		}
		for(size_t i = 0; i < arr1.size; i++){
			arr1.nums[i] -= arr2.nums[i];
		}
		return make_array(arr1);
}

Value mul_num_array(Arena *a, Value *left, Value *right){
		double num = left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;
		Array  arr = left->valueType == VALUE_ARRAY ? left->value.array : right->value.array;

    Array new_arr = clone_array(a, &arr);

		for(size_t i = 0; i < arr.size; i++){
			new_arr.nums[i] *= num;
		}
		return make_array(new_arr);
}

Value div_array_num(Arena *a, Value *left, Value *right){
		Array  arr = left->value.array;
		double num = right->value.number;

    Array new_arr = clone_array(a, &arr);

		for(size_t i = 0; i < arr.size; i++){
			new_arr.nums[i] /= num;
		}
		return make_array(new_arr);
}


// ------------- Polynomials --------------------

Value add_num_poly(Arena *a, Value *left, Value *right){
		double num       = left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;
		Polynomial  poly = left->valueType == VALUE_POLYNOMIAL ? left->value.poly : right->value.poly;

		Polynomial pnew = clone_poly(a, &poly);

		pnew.coeffs[0] += num;
		return make_poly(pnew);
}

Value add_poly_poly(Arena *a, Value *left, Value *right){
		Polynomial p1    = left->value.poly;
		Polynomial p2    = right->value.poly;
		size_t max_size  = MAX(p1.num_coeffs, p2.num_coeffs);

		Polynomial pnew = alloc_poly(a, max_size - 1);

		for(size_t i = 0; i < max_size; i++){
			double v1 = 0;
			double v2 = 0;
			if(i < p1.num_coeffs) v1 = p1.coeffs[i];
			if(i < p2.num_coeffs) v2 = p2.coeffs[i];
			pnew.coeffs[i] = v1 + v2;
		}

		return make_poly(pnew);
}

Value sub_num_poly(Arena *a, Value *left, Value *right){
		double num          = left->value.number;
		Polynomial  poly    = right->value.poly;

    Polynomial new_poly = scale_poly(a, &poly, -1.0);

		new_poly.coeffs[0] = num + new_poly.coeffs[0];
		return make_poly(new_poly);
}

Value sub_poly_num(Arena *a, Value *left, Value *right){
		Polynomial  poly    = left->value.poly;
		double num          = right->value.number;

    Polynomial new_poly = clone_poly(a, &poly);

		new_poly.coeffs[0] -= num;
		return make_poly(new_poly);
}



Value sub_poly_poly(Arena *a, Value *left, Value *right){
		Polynomial p1    = left->value.poly;
		Polynomial p2    = right->value.poly;
		size_t max_size  = MAX(p1.num_coeffs, p2.num_coeffs);

		Polynomial pnew = alloc_poly(a, max_size - 1);

		for(size_t i = 0; i < max_size; i++){
			double v1 = 0;
			double v2 = 0;
			if(i < p1.num_coeffs) v1 = p1.coeffs[i];
			if(i < p2.num_coeffs) v2 = p2.coeffs[i];
			pnew.coeffs[i] = v1 - v2;
		}

		return make_poly(pnew);
}

Value mul_poly_poly(Arena *a, Value *left, Value *right){
		Polynomial p1   = left->value.poly;
		Polynomial p2   = right->value.poly;

    size_t new_deg  = p1.deg + p2.deg;
		Polynomial pnew = alloc_poly(a, new_deg);

		for(size_t i = 0; i < p1.num_coeffs; i++){
      for(size_t j = 0; j < p2.num_coeffs; j++){
        pnew.coeffs[i + j] += p1.coeffs[i] * p2.coeffs[j];
      }
		}
    
		return make_poly(pnew);
}

Value mul_num_poly(Arena *a, Value *left, Value *right){
		Polynomial p = left->valueType == VALUE_POLYNOMIAL ? left->value.poly : right->value.poly;
		double num   = left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;

		Polynomial pnew = scale_poly(a, &p, num);
		return make_poly(pnew);
}


Value div_poly_poly(Arena *a, Value *left, Value *right){
		Polynomial p1 = left->value.poly;
		Polynomial p2 = right->value.poly;

		TransferFunction tf;
    tf.num = clone_poly(a, &p1);
    tf.den = clone_poly(a, &p2);
    
		return make_transfer_function(tf_simplify(a, &tf));
}


Value ordered_div_num_poly(Arena *a, Value *left, Value *right){
		double num   = left->value.number;
    Polynomial p = right->value.poly; 

		Polynomial pnew = poly_from_num(a, num);

    TransferFunction tf = {
      .num = pnew,
      .den = p
    };

		return make_transfer_function(tf_simplify(a, &tf));
}

Value ordered_div_poly_num(Arena *a, Value *left, Value *right){
		Polynomial p = left->valueType == VALUE_POLYNOMIAL ? left->value.poly : right->value.poly;
		double num   = left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;

    Value poly_value = make_poly(p);
    Value inv_num    = make_num(1.0/num);

    return mul_num_poly(a, &inv_num, &poly_value);
}


// ------------- Transfer Functions --------------------

Value add_tf_tf(Arena *a, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;

    // A     C
    // -  +  -
    // B     D

    Value A = make_poly(tf1.num);
    Value B = make_poly(tf1.den);
    Value C = make_poly(tf2.num);
    Value D = make_poly(tf2.den);

    Value AD = mul_poly_poly(a, &A, &D);
    Value BC = mul_poly_poly(a, &B, &C);
    Value BD = mul_poly_poly(a, &B, &D);

    Value AD_BC = add_poly_poly(a, &AD, &BC);

    TransferFunction tf = {
      .num = AD_BC.value.poly,
      .den = BD.value.poly
    };

    return make_transfer_function(tf_simplify(a, &tf));
}

Value sub_tf_tf(Arena *a, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;

    // A     C
    // -  -  -
    // B     D

    Value A = make_poly(tf1.num);
    Value B = make_poly(tf1.den);
    Value C = make_poly(tf2.num);
    Value D = make_poly(tf2.den);

    Value AD = mul_poly_poly(a, &A, &D);
    Value BC = mul_poly_poly(a, &B, &C);
    Value BD = mul_poly_poly(a, &B, &D);

    Value AD_BC = sub_poly_poly(a, &AD, &BC);

    TransferFunction tf = {
      .num = AD_BC.value.poly,
      .den = BD.value.poly
    };

    return make_transfer_function(tf_simplify(a, &tf));
}

Value mul_tf_tf(Arena *a, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;

    // A     C
    // -  *  -
    // B     D

    Value A = make_poly(tf1.num);
    Value B = make_poly(tf1.den);
    Value C = make_poly(tf2.num);
    Value D = make_poly(tf2.den);

    Value AC = mul_poly_poly(a, &A, &C);
    Value BD = mul_poly_poly(a, &B, &D);

    TransferFunction tf = {
      .num = AC.value.poly,
      .den = BD.value.poly
    };

    return make_transfer_function(tf_simplify(a, &tf));
}

Value mul_tf_poly(Arena *a, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    Polynomial poly     = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.poly : left->value.poly;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return mul_tf_tf(a, &tf_val, &poly_tf_val);
}

Value add_tf_poly(Arena *a, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    Polynomial poly     = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.poly : left->value.poly;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return add_tf_tf(a, &tf_val, &poly_tf_val);
}

Value sub_tf_poly(Arena *a, Value *left, Value *right){
    TransferFunction tf = left->value.tf;
    Polynomial poly     = right->value.poly;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return sub_tf_tf(a, &tf_val, &poly_tf_val);
}

Value sub_poly_tf(Arena *a, Value *left, Value *right){
    Polynomial poly     = left->value.poly;
    TransferFunction tf = right->value.tf;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return sub_tf_tf(a, &poly_tf_val, &tf_val);
}

Value add_num_tf(Arena *a, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    double num          = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.number : left->value.number;

		TransferFunction num_tf = tf_from_num(a, num);
		Value tf_val            = make_transfer_function(tf);
		Value num_tf_val        = make_transfer_function(num_tf);

    return add_tf_tf(a, &tf_val, &num_tf_val);
}


Value sub_tf_num(Arena *a, Value *left, Value *right){
    TransferFunction tf = left->value.tf;
    double num          = right->value.number;

		TransferFunction num_tf = tf_from_num(a, num);
		Value tf_val            = make_transfer_function(tf);
		Value num_tf_val        = make_transfer_function(num_tf);

    return sub_tf_tf(a, &tf_val, &num_tf_val);
}
Value sub_num_tf(Arena *a, Value *left, Value *right){
    double num          = left->value.number;
    TransferFunction tf = right->value.tf;

		TransferFunction num_tf = tf_from_num(a, num);
		Value tf_val            = make_transfer_function(tf);
		Value num_tf_val        = make_transfer_function(num_tf);

    return sub_tf_tf(a, &num_tf_val, &tf_val);
}
Value mul_num_tf(Arena *a, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    double num          = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.number : left->value.number;

		TransferFunction new_tf = scale_tf(a, &tf, num);
    return make_transfer_function(new_tf);
}
Value div_num_tf(Arena *a, Value *left, Value *right){
    double num          = left->value.number;
    TransferFunction tf = right->value.tf;

		TransferFunction inv_tf = tf_invert(a, &tf);
		TransferFunction new_tf = scale_tf(a, &inv_tf, num);

    return make_transfer_function(new_tf);
}

Value div_tf_num(Arena *a, Value *left, Value *right){
    double num          = left->value.number;
    TransferFunction tf = right->value.tf;

		TransferFunction new_tf = scale_tf(a, &tf, 1.0/num);

    return make_transfer_function(new_tf);
}

Value div_tf_tf(Arena *a, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;

		TransferFunction inv_tf2 = tf_invert(a, &tf2);

		Value tf1_val     = make_transfer_function(tf1);
		Value inv_tf2_val = make_transfer_function(inv_tf2);

    return mul_tf_tf(a, &tf1_val, &inv_tf2_val);
}

Value div_poly_tf(Arena *a, Value *left, Value *right){
		Polynomial p        = left->value.poly;
		TransferFunction tf = right->value.tf;

		TransferFunction tf_inv  = tf_invert(a, &tf);
		TransferFunction tf_poly = tf_from_poly(a, &p);
		
		Value tf_inv_val = make_transfer_function(tf_inv);
		Value tf_poly_val = make_transfer_function(tf_poly);

		return mul_tf_tf(a, &tf_inv_val, &tf_poly_val);
}

Value div_tf_poly(Arena *a, Value *left, Value *right){
		TransferFunction tf = left->value.tf;
		Polynomial p        = right->value.poly;

		TransferFunction tf_poly     = tf_from_poly(a, &p);
		TransferFunction tf_poly_inv = tf_invert(a, &tf_poly);
		
		Value tf_poly_val     = make_transfer_function(tf_poly);
		Value tf_poly_inv_val = make_transfer_function(tf_poly_inv);

		return mul_tf_tf(a, &tf_poly_inv_val, &tf_poly_val);
}

// ----------- Strings ------------------

Value cat_num_str(Arena *a, Value *left, Value *right){
  double num    = left->value.number;
  char *str     = right->value.str;
  char *num_str = string_from_num(a, num);
  return make_string(cat_strings(a, num_str, str));
}

Value cat_str_num(Arena *a, Value *left, Value *right){
  char *str  = left->value.str;
  double num = right->value.number;
  char *num_str = string_from_num(a, num);
  return make_string(cat_strings(a, str, num_str));
}

Value cat_str_str(Arena *a, Value *left, Value *right){
  char *str1 = left->value.str;
  char *str2 = right->value.str;
  return make_string(cat_strings(a, str1, str2));
}
Value eq_str_str(Arena *a, Value *left, Value *right){
		return make_num(strcmp(left->value.str, right->value.str) == 0);
}




static const BinaryFn binop_dispatch[OP_TYPE_LIST_COUNT][VALUE_TYPE_COUNT][VALUE_TYPE_COUNT] = {
	[OP_PLUS] = {
		[VALUE_NUMBER]              = {
			[VALUE_NUMBER]            = add_scalar_scalar,
			[VALUE_COMPLEX]           = add_scalar_scalar,
			[VALUE_ARRAY]             = add_num_array,
			[VALUE_POLYNOMIAL]        = add_num_poly,
			[VALUE_TRANSFER_FUNCTION] = add_num_tf,
			[VALUE_STRING]            = cat_num_str,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX]              = {
			[VALUE_NUMBER]            = add_scalar_scalar,
			[VALUE_COMPLEX]           = add_scalar_scalar,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_STRING]            = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_ARRAY] = {
			[VALUE_NUMBER]            = add_num_array,
			[VALUE_ARRAY]             = add_array_array,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = add_num_poly,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = add_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = add_tf_poly,
			[VALUE_NAN]               = op_with_nan,
		},
    [VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = add_num_tf,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = add_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = add_tf_tf,
			[VALUE_NAN]               = op_with_nan,
    },
		[VALUE_STRING] = {
			[VALUE_NUMBER]            = cat_str_num,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_STRING]            = cat_str_str,    
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	[OP_MINUS] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]            = sub_scalar_scalar,
			[VALUE_COMPLEX]           = sub_scalar_scalar,
			[VALUE_ARRAY]             = sub_num_array,
			[VALUE_POLYNOMIAL]        = sub_num_poly,
			[VALUE_TRANSFER_FUNCTION] = sub_num_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX]              = {
			[VALUE_NUMBER]            = sub_scalar_scalar,
			[VALUE_COMPLEX]           = sub_scalar_scalar,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_STRING]            = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_ARRAY] = {
			[VALUE_NUMBER]            = sub_array_num,
			[VALUE_ARRAY]             = sub_array_array,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = sub_poly_num,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = sub_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = sub_poly_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = sub_tf_num,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = sub_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = sub_tf_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	[OP_MUL] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]            = mul_scalar_scalar,
			[VALUE_COMPLEX]           = mul_scalar_scalar,
			[VALUE_ARRAY]             = mul_num_array,
			[VALUE_POLYNOMIAL]        = mul_num_poly,
			[VALUE_TRANSFER_FUNCTION] = mul_num_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]            = mul_scalar_scalar,
			[VALUE_COMPLEX]           = mul_scalar_scalar,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_ARRAY] = {
			[VALUE_NUMBER]            = mul_num_array,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = mul_num_poly,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = mul_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = mul_tf_poly,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = mul_num_tf,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = mul_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = mul_tf_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	[OP_DIV] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]            = div_scalar_scalar,
			[VALUE_COMPLEX]           = div_scalar_scalar,
			[VALUE_ARRAY]             = op_with_nan,
      [VALUE_POLYNOMIAL]        = ordered_div_num_poly,
			[VALUE_TRANSFER_FUNCTION] = div_num_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]            = div_scalar_scalar,
			[VALUE_COMPLEX]           = div_scalar_scalar,
			[VALUE_ARRAY]             = op_with_nan,
      [VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_ARRAY] = {
			[VALUE_NUMBER]            = div_array_num,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = ordered_div_poly_num,
			[VALUE_ARRAY]             = op_with_nan,
      [VALUE_POLYNOMIAL]        = div_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = div_poly_tf,
			[VALUE_NAN]               = op_with_nan,
    },
		[VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = div_tf_num,
			[VALUE_ARRAY]             = op_with_nan,
      [VALUE_POLYNOMIAL]        = div_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = div_tf_tf,
			[VALUE_NAN]               = op_with_nan,
    },
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	[OP_POW] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]  = pow_scalar_scalar,
			[VALUE_COMPLEX] = pow_scalar_scalar,
			[VALUE_ARRAY]   = op_with_nan,
			[VALUE_NAN]     = op_with_nan,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]  = pow_scalar_scalar,
			[VALUE_COMPLEX] = pow_scalar_scalar,
			[VALUE_ARRAY]   = op_with_nan,
			[VALUE_NAN]     = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_ARRAY]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	// Comparisons

	[OP_DBL_EQ] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]  = eq_scalar_scalar,
			[VALUE_COMPLEX] = eq_scalar_scalar,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]  = eq_scalar_scalar,
			[VALUE_COMPLEX] = eq_scalar_scalar,
		},
		[VALUE_STRING] = {
			[VALUE_STRING] = eq_str_str,
		}
	},

	[OP_LT] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = lt_num_num,
		}
	},

	[OP_GT] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = gt_num_num,
		}
	},

	[OP_GEQ] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = geq_num_num,
		}
	},

	[OP_LEQ] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = leq_num_num,
		}
	},

};


//====================================
//
//   Native functions
//
//====================================

//------------ System ------------

Value native_assert(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  *silent    = 1;
	Value cond = argv[0];
	Value msg  = argv[1];
	if(msg.valueType != VALUE_STRING){
		fprintf(stderr, "Second argument must me a string\n");
		(*e->failed_asserts)++;
		return make_nan();
	}
	if(!cond.value.number){
		fprintf(stderr, "FAIL: %s: %s\n", script, msg.value.str);
		(*e->failed_asserts)++;
	}
  return make_num(cond.value.number);
}

Value native_ls(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  env_list_variables(e);
  *silent = 1;
  return make_num(0);
}

void print_value(Arena *ta, Environment *e, Value *value);
Value native_print(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value val = argv[0];
    print_value(a, e, &val);
    *silent = 1;
    return make_num(0);
}

int import_script(Arena *ta, Environment *e, char* importer, char *path);
Value native_import(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value val = argv[0];
  if(val.valueType != VALUE_STRING){
    char buf[1024];
    printf("Importing a module expects a string.\n");
    *silent = 1;
    return make_num(0);
  }
  import_script(a, e, script, val.value.str);
  *silent = 1;
  return make_num(0);
}



//------------ Math --------------

Value native_abs(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
	if(argv[0].valueType == VALUE_COMPLEX) return make_num(cabs(argv[0].value.complex_number));
  return make_num(fabs(argv[0].value.number));
}
Value native_real(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(creal(argv[0].value.complex_number));
}
Value native_imag(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(cimag(argv[0].value.complex_number));
}
Value native_arg(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(carg(argv[0].value.complex_number));
}
Value native_log(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(log(argv[0].value.number));
}
Value native_exp(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(exp(argv[0].value.number));
}
Value native_sin(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(sin(argv[0].value.number));
}
Value native_cos(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
  return make_num(cos(argv[0].value.number));
}
Value native_sqr(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(argv[0].value.number * argv[0].value.number);
}
Value native_sqrt(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  return make_num(sqrt(argv[0].value.number));
}
Value native_range(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){    
	Value from_val = argv[0];
	Value to_val   = argv[1];
	if(from_val.valueType != VALUE_NUMBER || to_val.valueType != VALUE_NUMBER){
		printf("Expected 2 real numbers. Example: range(1,5) = [1,2,3,4]\n");
		return make_nan();
	}
	int from = floor(from_val.value.number);
	int to   = ceil(to_val.value.number);
	int n    = to - from;
	int nele = abs(n);

	Array arr = {
		.size = nele,
		.nums = arena_alloc(a, nele * sizeof(double))
	};

	int step = (from < to) - (from > to);

	for(int i = 0; i < nele; i++) {
		arr.nums[i] = from + (i * step);
	}

	return make_array(arr);
}
Value native_poly(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value val = argv[0];
  if(val.valueType == VALUE_ARRAY){
    int err;
    Polynomial p = poly_new(a, val.value.array.nums, val.value.array.size);
    poly_update_degree(&p);
    return make_poly(p);
  }
  printf("Unsupported operation!\n");
  return make_nan();
}
Value native_polymod(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  Value second = argv[1];
  if(first.valueType == VALUE_POLYNOMIAL && second.valueType == VALUE_POLYNOMIAL){
    Polynomial rem;
    poly_divmod(a, first.value.poly, second.value.poly, NULL, &rem);
    return make_poly(rem);
  }
  printf("Unsupported operation, expected a polynomial!\n");
  return make_nan();
}
Value native_polydiv(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  Value second = argv[1];
  if(first.valueType == VALUE_POLYNOMIAL && second.valueType == VALUE_POLYNOMIAL){
    Polynomial div;
    poly_divmod(a, first.value.poly, second.value.poly, &div, NULL);
    return make_poly(div);
  }
  printf("Unsupported operation, expected a polynomial!\n");
  return make_nan();
}
Value native_polygcd(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  Value second = argv[1];
  if(first.valueType == VALUE_POLYNOMIAL && second.valueType == VALUE_POLYNOMIAL){
    Polynomial gcd = poly_gcd(a, first.value.poly, second.value.poly);
    return make_poly(gcd);
  }
  printf("Unsupported operation, expected a polynomial!\n");
  return make_nan();
}

Value native_poles(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  if(first.valueType == VALUE_TRANSFER_FUNCTION){
		TransferFunction tf = first.value.tf;
		double complex *roots = tf_poles(a, tf);

		Value *values = arena_alloc(a, tf.den.deg * sizeof(Value));
		for(size_t i = 0; i < tf.den.deg; i++){ values[i] = make_complex(roots[i]); }

		Tuple t = {
			.size = tf.den.deg,
			.items = values,
		};
		return make_tuple(t);
  }
  printf("Unsupported operation, expected a transfer function.\n");
  return make_nan();
}

Value native_complex(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
	Value re = argv[0];
	Value im = argv[1];
	if(re.valueType != VALUE_NUMBER || im.valueType != VALUE_NUMBER){
		printf("Expected 2 real numbers. Example: complex(1,2) = 1 + 2*j\n");
		return make_nan();
	}
	return make_complex(re.value.number + I * im.value.number);
}



static const NativeValueDispatch native_fns[] = {
  native_assert,
  native_ls,
  native_print,
  native_import,
  // Math
  native_abs,
	native_complex,
  native_real,
  native_imag,
  native_arg,
  native_log,
  native_exp,
  native_sin,
  native_cos,
  native_sqr,
  native_sqrt,
  native_range,
  // Poly
  native_poly,
  native_polymod,
  native_polydiv,
  native_polygcd,
	// TF
	native_poles,
};

static const char *native_fn_names[] = {
  "assert",
  "ls",
  "print",
  "import",
  // Math
  "abs",
  "complex",
  "real",
  "imag",
  "arg",
  "log",
  "exp",
  "sin",
  "cos",
  "sqr",
  "sqrt",
	"range",
  // Poly
  "poly",
  "polymod",
  "polydiv",
  "polygcd",
	// TF
	"poles"
};

static const size_t native_fn_argc[] = {
  2,
  0,
  1,
  1,
  // Math
  1,
	2,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
	2,
  // Poly
  1,
  2,
  2,
  2,
	// TF
	1,
};



Value resolve_variable(Arena *a, Environment *e, Value *literal){
    if(literal->valueType == VALUE_STRING){
      return *literal;
    }
		Variable *var = env_find_variable(e, literal->value.str);
		if(!var) {
			printf("Undefined variable %s\n", literal->value.str);
			return make_nan();
		}
		return *var->value;
}

// Evaluation

typedef enum {
	EXEC_OK,
	EXEC_ERROR,
	EXEC_RETURN,
} ExecStatus;

ExecStatus eval_stmt(Arena* ta, AST *ast, Environment *e, char *script_path, AST *stmt, Value *result_out);
Value eval_expr(Arena *a, Environment *e, char *script_path, AST *expr, int *silent);
ExecStatus eval_block( Arena *a, AST *ast, Environment *e, char *script_path, AST *first, Value *result_out);

Value eval_unop(Arena *a, Environment *e, AST *op, Value *arg_given){
	Value *arg  = arg_given;

	if(arg->valueType == VALUE_ID){
		*arg = resolve_variable(a, e, arg);
	}

	if( op->as.binop.op >= OP_TYPE_LIST_COUNT ||
			arg->valueType >= VALUE_TYPE_COUNT
			)
	{
		return make_nan();
	}

	UnaryFn fn = unop_dispatch[op->as.binop.op][arg->valueType];

	if(!fn){
		printf("Unsupported operation!\n");
		return make_nan();
	}

	return fn(a, arg);


	fprintf(stderr, "Unrecognized unary operator\n");
	exit(1);

}

Value eval_binop(Arena *a, Environment *e, AST *op, Value *raw_left, Value *raw_right){

	Value *left  = raw_left;
	Value *right = raw_right;

	// Resolve variables
	// TODO: Check if it begins  in quotation  marks?  For  now, assume
	//       all strings  are  variable  names
	if(left->valueType == VALUE_ID){
		*left = resolve_variable(a, e, left);
	}
	if(right->valueType == VALUE_ID){
		*right = resolve_variable(a, e, right);
	}

	if( op->as.binop.op >= OP_TYPE_LIST_COUNT ||
			left->valueType >= VALUE_TYPE_COUNT  ||
			right->valueType >= VALUE_TYPE_COUNT
			)
	{
		return make_nan();
	}

	BinaryFn fn = binop_dispatch[op->as.binop.op][left->valueType][right->valueType];

	if(!fn){
		printf("Unsupported binary operation!\n");
		return make_nan();
	}

	return fn(a, left, right);


	fprintf(stderr, "Expected 2 numbers, got something else!\n");
	exit(1);

}


Environment *push_env(Environment *e){
  Arena *fa = e->frame;

	Environment *new_env = arena_alloc(fa, sizeof(Environment));

  new_env->persistent     = e->persistent;
	new_env->parent         = e;
  new_env->frame          = e->frame;
  new_env->num_vars       = 0;
  new_env->vars           = arena_alloc(fa, 32 * sizeof(Variable));
	new_env->failed_asserts = e->failed_asserts;
	new_env->exe_path       = e->exe_path;

	return new_env;
}

Value eval_function(Arena *ta, Environment *e, AST *fn_call, char *script_path, int *silent){
	int argc      = fn_call->as.fn_call.num_arguments;
	char *fn_name = fn_call->as.fn_call.fn_name;

	// Evaluating arguments in the base environment

	Value *evaluated = arena_alloc(ta, argc * sizeof(Value));
	if(argc > 0){
		size_t i = 0;
		for(
			AST *arg  = fn_call->as.fn_call.first_arg;
			arg      != NULL;
			arg       = arg->next
		){
			Value val    = eval_expr(ta, e, script_path, arg, silent);
			evaluated[i] = val;
			i++;
		}
	}

	// Get the function variable from the base environment

	Variable *fun_var = env_find_variable(e, fn_name);
	if(!fun_var) {
		printf("Undefined function %s\n", fn_call->as.fn_call.fn_name);
		return make_nan();
	}
	Value *fun = fun_var->value;

	// Sanity checks

	if (fun->valueType != VALUE_FUNCTION) {
		fprintf(stderr, "%s is not a function\n", fn_name);
		return make_nan();
	}

  size_t num_args = 0;
  if(fun->value.fun.kind == FN_NATIVE){
    num_args = fun->value.fun.native_data.num_arguments_native;
  }else{
    num_args = fun->value.fun.definition->as.fn_def.num_args;
  }

	size_t expected = num_args;

	if ((size_t)argc != expected) {
		fprintf(stderr,
				"%s expects %zu arguments, got %d\n",
				fn_name, expected, argc);
		return make_nan();
	}

  if(fun->value.fun.kind == FN_NATIVE){
    int idx = fun->value.fun.native_data.native_index;
    if(idx < 0 || idx >= LEN(native_fns)){
      fprintf(stderr, "%s not registered\n", fn_name );
      return make_nan();
    }
    NativeValueDispatch dispatch = native_fns[idx];
    Value result = dispatch(ta, e, script_path, argc, evaluated, silent);
    return result;
  }


	// Push a new environment for the function scope

  size_t scratch = arena_save(e->frame);
	Environment *new_env = push_env(e);

	// Set its argument values to those evaluated before

	size_t i = 0;
	for(
			AST *arg      = fn_call->as.fn_call.first_arg,
			    *arg_def  = fun->value.fun.definition->as.fn_def.params;
			arg          != NULL;
			arg           = arg->next,
			arg_def       = arg_def->next
		 ){
		env_set_variable(new_env, arg_def->as.literal.value.id, &evaluated[i]);
		i++;
	}

	Value result;
  AST *block     = fun->value.fun.definition->as.fn_def.block;
  ExecStatus out = eval_block(ta, fn_call, new_env, script_path, block, &result);

  if(out == EXEC_ERROR){
    arena_restore(e->frame, scratch);
    return make_nan();
  }

  Value escaped = value_clone(ta, &result);
  arena_restore(e->frame, scratch);
	return escaped;
}

void print_value(Arena *ta, Environment *e, Value *value){
  printf("\n");
	switch(value->valueType){
		case VALUE_ID:
			{
				Value resolved = resolve_variable(ta, e, value);
				if(resolved.valueType == VALUE_NAN){
					printf("%s\n", value->value.id);
				}else{
					print_value(ta, e, &resolved);
				}
				break;
			}
		case VALUE_NAN:
			{
				printf("nan\n");
				break;
			}
		case VALUE_NUMBER:
			{
				printf("%.4f\n", value->value.number);
				break;
			}
		case VALUE_COMPLEX: 
			{
				if(fabs(cimag(value->value.complex_number)) > 1e-12){
					if(cimag(value->value.complex_number) > 0){
						printf(
								"%.4f + %.4f * i\n", 
								creal(value->value.complex_number),
								cimag(value->value.complex_number)
								);
						break;
					}
					printf(
							"%.4f - %.4f * i\n", 
							creal(value->value.complex_number),
							-cimag(value->value.complex_number)
							);
					break;
				}
				printf("%.4f\n", creal(value->value.complex_number));
				break;
			}
		case VALUE_STRING:
			{
        printf("%s\n", value->value.str);
				break;
			}
		case VALUE_ARRAY:
			{
				printf("[");
				for(size_t i = 0; i < value->value.poly.num_coeffs; i++){
					printf("%.2f", value->value.poly.coeffs[i]);
					if(i < value->value.poly.num_coeffs - 1){
						printf(", ");
					}
				}
				printf("]\n");
				break;
			}
		case VALUE_TUPLE:
			{
				printf("(\n");
				for(size_t i = 0; i < value->value.tuple.size; i++){
          print_value(ta, e, &value->value.tuple.items[i]);
				}
				printf(")\n");
				break;
			}
		case VALUE_POLYNOMIAL:
			{
        char buf[1024];
        poly_repr(ta, &value->value.poly, buf, "x");
        printf("%s\n", buf);
				break;
			}
		case VALUE_FUNCTION:
			{
        char buf[1024];
        if(value->value.fun.kind == FN_NATIVE){
          function_repr(
              ta, 
              value->value.fun.native_data.num_arguments_native,
              native_fn_names[value->value.fun.native_data.native_index],
              buf
          );
        }else{
          function_repr(
              ta, 
              value->value.fun.definition->as.fn_def.num_args,
              value->value.fun.definition->as.fn_def.id,
              buf
          );
        }
        printf("%s%s\n", value->value.fun.kind == FN_NATIVE ? "native " : "", buf);
				break;
			}
		case VALUE_TRANSFER_FUNCTION:
			{
        char buf[1024];
        transfer_function_repr(ta, &value->value.tf, buf, "x");
        printf("%s\n", buf);
				break;
			}
		default:
			{
				break;
			}
	}
  printf("\n");
}

int run_program(Arena *ta, Environment *e, FILE *f, char *script_path);

char *path_join_dir(Arena *a, const char* file, const char *rel){
	// Example:
	// a/b/script.tc + x.tc -> a/b/x.tc
	// script.tx -> current directory

	const char *last_slash = file ? strrchr(file, '/') : NULL;
	size_t dir_len         = last_slash ? (size_t)(last_slash - file) : 1;
	const char *dir        = last_slash ? file : ".";

	size_t len = dir_len + 1 + strlen(rel);
	char *out  = arena_alloc(a, len + 1);
	snprintf(out, len + 1, "%.*s/%s", (int)dir_len, dir, rel);
	return out;

}

char *resolve_path(Arena *a, char* script_path, char *exe_path, char *path){
  char *candidate = NULL;

  // base.tc            -> executable_path/libs/base.tc
  // ./folder/script.tc -> caller_path/folder/script.tc
  // Absolute paths are kept

	// Absolute
  if(path[0] == '/'){ return clone_string(a, path); }

  if(path[0] == '~' && path[1] == '/'){
    const char *home = getenv("HOME");
    if(!home) return NULL;

    // Example:
    // path     = ~/scripts/a.tc              (14 chars)
    // home     = /home/user                  (10 chars)
    // combined = /home/user/scripts/a.tc     (23 chars) -> len1 + len2 - 1

    size_t len       = strlen(home) + strlen(path) - 1;
    candidate        = arena_alloc(a, len + 1);
    sprintf(candidate, "%s%s", home, path + 1);
    candidate[len]   = '\0';
    return candidate;
  }

	if(
			strncmp(path, "./", 2)  == 0 ||
			strncmp(path, "../", 3) == 0
		){ return path_join_dir(a, script_path, path); }

	char *lib = path_join_dir(a, exe_path, "libs/x"); // Placeholder so that we can reuse this fn
	lib[strlen(lib) - 1] = '\0';
	return path_join_dir(a, lib, path);
}

int import_script(Arena *ta, Environment *e, char* importer, char *path){ 
  char *resolved_path = resolve_path(ta, importer, e->exe_path, path);
  FILE *f             = fopen(resolved_path, "r");
  if(f == NULL){
    perror(path);
    return 1;
  }
  int code = run_program(ta, e, f, resolved_path);
  fclose(f);
  return code;
}


Value unary_callable_poly_num(Arena *a, Value *caller, Value *arg){
  return make_num(poly_horner(caller->value.poly, arg->value.number));
}

Value unary_callable_tf_num(Arena *a, Value *caller, Value *arg){
  double num_val = poly_horner(caller->value.tf.num, arg->value.number);
  double den_val = poly_horner(caller->value.tf.den, arg->value.number);
  if(ISZERO(den_val)) return make_nan();
  return make_num(num_val/den_val);
}

UnaryValueDispatch callable_values_dispatch[VALUE_TYPE_COUNT][VALUE_TYPE_COUNT] = {
  [VALUE_POLYNOMIAL] = {
    [VALUE_NUMBER] = unary_callable_poly_num,
  },
  [VALUE_TRANSFER_FUNCTION] = {
    [VALUE_NUMBER] = unary_callable_tf_num,
  },
};



void initialize_native_fns(Environment *e){
  for(size_t i = 0; i < LEN(native_fns); i++){
    NativeFunctionData data = {
      .num_arguments_native = native_fn_argc[i],
      .native_index         = i
    };
    Function native_fn = {
      .kind        = FN_NATIVE,
      .native_data = data
    };
    Value fn = make_function(native_fn);
    env_set_variable(e, native_fn_names[i], &fn);
  }
}

Value eval_func_call(Arena *ta, Environment *e, char *script_path, AST *fn, int *silent){

  char *name  = fn->as.fn_call.fn_name;
  size_t argc = fn->as.fn_call.num_arguments;

	// Check if it is a user-defined function

	Variable *var = env_find_variable(e, name);
	if(var && var->value->valueType == VALUE_FUNCTION){
		return eval_function(ta, e, fn, script_path, silent);
	}


  if(argc == 1){
		Value val = eval_expr(ta, e, script_path, fn->as.fn_call.first_arg, silent);

    // ---------- Check if there's a suitable variable with name fn_name -----------

    Variable *var = env_find_variable(e,  fn->as.fn_call.fn_name);
    if(var != NULL){
      if(var->value->valueType >= VALUE_TYPE_COUNT ||
         val.valueType >= VALUE_TYPE_COUNT)
      {
        printf("Unsupported operation!\n");
        return make_nan();
      }

      UnaryValueDispatch dispatch = 
        callable_values_dispatch[var->value->valueType][val.valueType];


      if(!dispatch){
        printf("Unsupported operation!\n");
        return make_nan();
      }

      return dispatch(ta, var->value, &val);
       
    }

  }

	return make_nan();

}

Value eval_indexing(Arena *ta, Environment *e, char *script_path, AST *indexing, int *silent){

  char *name = indexing->as.indexing.var_name;
	Value val = eval_expr(ta, e, script_path, indexing->as.indexing.index_expr, silent);
	if(val.valueType != VALUE_NUMBER){
		printf("Indices must be numbers.%s\n", name);
		return make_nan();
	}
  int idx = (int)val.value.number;

	// Check if it is an indexable variable

	Variable *var = env_find_variable(e, name);
	if(!var){
		printf("Undefined variable %s\n", name);
		return make_nan();
	}

	if(
			!(
					var->value->valueType == VALUE_ARRAY ||
					var->value->valueType == VALUE_TUPLE 
			)
		)
	{
		printf("Can't index from variable %s\n", name);
		return make_nan();
	}

	if(var->value->valueType == VALUE_ARRAY){
		int size = (int)var->value->value.array.size;
		if(idx >= size){
			printf("Index out of bounds: %s\n", name);
			return make_nan();
		}
		return make_num(var->value->value.array.nums[modulo(idx, size)]);
	}

	if(var->value->valueType == VALUE_TUPLE){
		int size = (int)var->value->value.tuple.size;
		if(idx >= size){
			printf("Index out of bounds: %s\n", name);
			return make_nan();
		}
		return var->value->value.tuple.items[modulo(idx, size)];
	}

	return make_nan();

}


Value eval_expr(Arena *ta, Environment *e, char *script_path, AST *expr, int *silent){
	Value value = {0};

	switch(expr->type){
		case AST_ID:
			{
				value = resolve_variable(ta, e, &expr->as.literal);
				break;
			}
		case AST_LITERAL:
			{

				Value literal = expr->as.literal;
				if(literal.valueType == VALUE_STRING){
					literal = resolve_variable(ta, e, &expr->as.literal);
				}
				value = literal;
				break;
			}
		case AST_ARRAY:
			{
				value.valueType        = VALUE_ARRAY;
				value.value.array.size = expr->as.array.size;
				value.value.array.nums = arena_alloc(ta, expr->as.array.size * sizeof(double));
				AST *item              = expr->as.array.first;
				size_t i               = 0;
				while(item != NULL){
					Value item_value = eval_expr(ta, e, script_path, item, silent);
					// TODO: check if it really is a number
					value.value.array.nums[i++] = item_value.value.number;
					item                        = item->next;
				}
				break;
			}
		case AST_TUPLE:
			{
				value.valueType        = VALUE_TUPLE;
				value.value.tuple.size = expr->as.tuple.size;
				value.value.tuple.items = arena_alloc(ta, expr->as.tuple.size * sizeof(Value));
        size_t i = 0;
        for(AST *item = expr->as.tuple.first; item != NULL; item = item->next){
					Value item_value = eval_expr(ta, e, script_path, item, silent);
					value.value.tuple.items[i] = item_value;
          i++;
        }
				break;
			}
		case AST_BINARY_OP:
			{
				Value left  = eval_expr(ta,  e, script_path, expr->as.binop.left, silent);
				Value right = eval_expr(ta,  e, script_path, expr->as.binop.right, silent);
				value       = eval_binop(ta, e, expr, &left, &right);
				break;
			}
		case AST_UNARY_OP:
			{
				Value arg = eval_expr(ta,  e, script_path, expr->as.unop.arg, silent);
				value     = eval_unop(ta, e, expr, &arg);
				break;
			}
		case AST_FN_CALL:
			{
				value = eval_func_call(ta, e, script_path, expr, silent);
				break;
			}
		case AST_INDEXING:
			{
				value = eval_indexing(ta, e, script_path, expr, silent);
				break;
			}
		default:
			{
				fprintf(stderr, "Unreachable [eval_expr]\n");
				exit(1);
			}
	}

	return value;
}

ExecStatus eval_block(
    Arena *ta,
    AST *ast,
    Environment *e,
    char *script_path,
    AST *block,
    Value *result_out
) {
    Value result = make_num(0);

    for (AST *stmt = block->as.statement_list.first; stmt != NULL; stmt = stmt->next) {

        if (stmt->statement_type == STATEMENT_EXPR) {
            int silent = 1;
            result = eval_expr( ta, e, script_path, stmt, &silent);
        } else {
            int err = eval_stmt(ta, ast, e, script_path, stmt, result_out);
            if (err != 0) return err;

            result = make_num(0);
        }
    }

    if (result_out)
        *result_out = result;

    return 0;
}

ExecStatus eval_stmt(
		Arena* ta,
		AST *ast,
		Environment *e,
		char *script_path,
		AST *stmt,
		Value *result_out
		){
  int silent = 0;

	if(stmt->statement_type == STATEMENT_EXPR){
    SCRATCH_SCOPE(ta){
      Value result = eval_expr(ta, e, script_path, stmt, &silent);
      if(!silent) print_value(ta, e, &result);
    }
		return 0;
	}

	if(stmt->statement_type == STATEMENT_DECL_ASSIGN){
		char *dst = stmt->as.statement.dst;
		// Check if it hasn't been defined before
    if(env_find_local(e, dst) != NULL){
			printf("Variable %s has already been defined\n", dst);
			return 1;
    }

		//  TODO: Check if it's not null
    SCRATCH_SCOPE(ta){
      Value value = eval_expr(ta, e, script_path, stmt->as.statement.expr, &silent);
      Value saved = value_clone(env_storage(e), &value);
      env_set_variable(e, dst, &saved);
    }
		return 0;
	}

	if(stmt->statement_type == STATEMENT_ASSIGN){
		char *dst = stmt->as.statement.dst;
		// Check if it has been defined before
    if(env_find_local(e, dst) == NULL){
			printf("Variable %s has not been defined\n", dst);
			return 1;
    }

		//  TODO: Check if it's not null
    SCRATCH_SCOPE(ta){
      Value value = eval_expr(ta, e, script_path, stmt->as.statement.expr, &silent);
      Value saved = value_clone(env_storage(e), &value);
      env_set_variable(e, dst, &saved);
    }
		return 0;
	}

	if(stmt->statement_type == STATEMENT_FUNCTION_DEF){
		char *id = stmt->as.fn_def.id;

		// Check if it has been defined before
		if(env_find_variable(e, id) != NULL){
			printf("There's already a variable with the name: %s\n", id);
			return 1;
		}

    Function fun = {
      .kind       = FN_USER,
      .definition = stmt
    };
    Value value   = make_function(fun);
    env_set_variable(e, id, &value);

		return 0;
	}

  if(stmt->statement_type == STATEMENT_IF){
    Value cond = eval_expr(
        ta, e, script_path,
        stmt->as.if_stmt.condition, &silent
        );

    if (cond.valueType != VALUE_NUMBER) {
      fprintf(stderr, "Expected numeric condition\n");
      return 1;
    }

    int cond_true = cond.value.number != 0.0;

    AST *branch = cond_true
      ? stmt->as.if_stmt.if_block
      : stmt->as.if_stmt.else_block;

    if (branch == NULL)
      return 0;

    if (!cond_true && stmt->as.if_stmt.else_is_elif) {
      return eval_stmt(ta, ast, e, script_path, branch, result_out);
    }

    return eval_block(ta, ast, e, script_path, branch, result_out);
  }

  if(stmt->statement_type == STATEMENT_RETURN){
		if(!result_out) return EXEC_RETURN;
		if(!stmt->as.statement.expr){ *result_out = make_num(0); return EXEC_RETURN; }
		*result_out = eval_expr(ta, e, script_path, stmt->as.statement.expr, &silent);
		return EXEC_RETURN;
  }

	return 0;
}

int run_ast(Arena* ta, AST *ast, Environment *e, char *script_path){

	AST *stmt = ast->as.program.statement_list->as.statement_list.first;

	while(stmt != NULL){
		ExecStatus status = eval_stmt(ta, ast, e, script_path, stmt, NULL);
		if(status == EXEC_RETURN){
			printf("Return outside function body\n");
		}
		if(status != EXEC_OK) return status;
		stmt = stmt->next;
	}

	return 0;
}


int parse_source(Environment *e,  const char *source, size_t source_len, AST *ast){
	// Null-terminated

  // --------- Commands that require parsing -----------

	Token  *tokens    = arena_alloc(e->persistent, (source_len + 1) * sizeof(Token));
	size_t num_tokens = 0;
	int    err        = 0;
	err = tokenize(e->persistent, source, source_len, tokens, &num_tokens);
	if(err != 0){
		fprintf(stderr, "Tokenization error\n");
		return err;
	}

  // for(size_t i = 0; i < num_tokens; i++){
  //   token_print(&tokens[i]);
  // }

	return parse_tokens(e->persistent, ast, tokens, num_tokens);
}

//============================== 
//
//    Helpers
//
//============================== 

size_t trim_end(char *str){
	// Ex:       this is a string      \0
	// Produces: this is a string\0
	// Assumes string to be null-terminated
	// Mutates the string in-place

	size_t size = strlen(str);
	for(int i = size - 1; i >= 0; i--){
		if(isspace(str[i])){
			str[i] = '\0';
		}else{
			break;
		}
	}

	return strlen(str);
}

//============================== 
//
//    Main
//
//============================== 

void print_usage(){
	printf("Usage: tctl [FILE]\n");
	printf("    Runs a tiny-ctrl script located at FILE\n");
	printf("    If a FILE argument isn't supplied, a repl is launched\n");
}

void cleanup(Arena *ta, Arena *a){
	arena_destroy(ta);
	arena_destroy(a);
}

int launch_repl(Arena *ta, Environment *e, char *script_path){
  char *buffer      = NULL; // Null-terminated
  size_t size       = 0;
  ssize_t nread     = 0;
  ssize_t last_good = 0;
  int parse_code    = 0;
  int stop          = 0;

  while(1){

    printf("tiny-ctrl> ");
    nread = getline(&buffer, &size, stdin);
    if(nread == -1){
      printf("\nNo lines were read. Exiting...\n");
      break;
    }

    // Trimming the endline characters
    last_good         = strcspn(buffer, "\r\n");
    buffer[last_good] = '\0';

    trim_end(buffer);

    // --------- Basic commands -----------

    if (strcmp(buffer, "") == 0)
      continue;

    if (strcmp(buffer, "q") == 0) {
      printf("\nExiting...\n");
      break;
    }

    // Tokenization and parsing use the permanent arena
    AST *ast   = arena_alloc(e->persistent, sizeof(AST));
    parse_code = parse_source(e,  buffer, strlen(buffer), ast);

    // Evaluation uses temporary memory
    SCRATCH_SCOPE(ta){
      if(parse_code == 0) run_ast(ta, ast, e, script_path);
    }
    if(parse_code != 0){
      printf("Syntax error\n");
      continue;
    }

  }
  free(buffer);
  return parse_code;
}

char *read_entire_file(size_t *out_len, FILE *f){
  if(fseek(f, 0, SEEK_END) != 0){
    perror("Error seeking to end of file");
    return NULL;
  }

  long file_size = ftell(f);
  if(file_size < 0){
    perror("Error telling file size");
    return NULL;
  }

  if (fseek(f, 0, SEEK_SET) != 0) {
    perror("Error seeking to start of file");
    return NULL;
  }

  char *buffer = malloc(file_size + 1);
  if(!buffer){
    perror("Memory allocation failed");
    return NULL;
  }

  size_t bytes_read = fread(buffer, 1, file_size, f);
  if (bytes_read < (size_t)file_size && ferror(f)) {
    perror("Error reading file");
    free(buffer);
    return NULL;
  }
  buffer[bytes_read] = '\0';

  if(out_len != NULL){
    *out_len = bytes_read;
  }

  return buffer;

}

int run_program(Arena *ta, Environment *e, FILE *f, char *script_path){
	size_t len         = 0;
	int parse_code     = 0;

  char *buffer = read_entire_file(&len, f);

  // Tokenization and parsing use the permanent arena
  AST *ast     = arena_alloc(e->persistent, sizeof(AST));
  parse_code   = parse_source(e,  buffer, len, ast);

  // ast_print(ast, 0);

  // Evaluation uses temporary memory
  SCRATCH_SCOPE(ta){
    if(parse_code == 0) run_ast(ta, ast, e, script_path);
  }

  free(buffer);
	if(*e->failed_asserts > 0){
    printf("Assertions failed: %zu...\n", *e->failed_asserts);
		return 1;
	}
  if(parse_code != 0){
    printf("Exiting with code %i...\n", parse_code);
  }
	return parse_code;
}


int main(int argc, char **argv){
	size_t size           = 0;
	ssize_t nread         = 0;
	ssize_t last_good     = 0;
	int parse_code        = 0;
	size_t failed_asserts = 0;

	Arena ta      = arena_create(1024 * 1024);      // 1 MB for now
	Arena pa      = arena_create(32 * 1024 * 1024); // 32 MB for now
	Arena fa      = arena_create(1 * 1024 * 1024); // Frame arena
	Environment e = env_create(&pa, &fa, &failed_asserts, argv[0]);

	if(argc < 2){
		// REPL
		int code = launch_repl(&ta, &e, argv[0]);
		cleanup(&ta, &pa);
		return code;
	}else{

		if(strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0){
			print_usage();
			cleanup(&ta, &pa);
			exit(0);
		}

		// Launch a program

		FILE *f = fopen(argv[1], "r");

		if(f != NULL){
			int code = run_program(&ta, &e, f, argv[1]);
			cleanup(&ta, &pa);
			return code;
		}else{
			fprintf(stderr, "Could not load file at %s\n", argv[1]);
		}

		fclose(f);

	}

	return 0;
}
