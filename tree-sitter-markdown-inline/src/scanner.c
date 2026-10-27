#include "tree_sitter/parser.h"

#ifdef _MSC_VER
#define UNUSED
#else
#define UNUSED __attribute__((unused))
#endif

// For explanation of the tokens see grammar.js
typedef enum {
    ERROR,
    TRIGGER_ERROR,
    CODE_SPAN_START,
    CODE_SPAN_CLOSE,
    EMPHASIS_OPEN_STAR,
    EMPHASIS_OPEN_UNDERSCORE,
    EMPHASIS_CLOSE_STAR,
    EMPHASIS_CLOSE_UNDERSCORE,
    LAST_TOKEN_WHITESPACE,
    LAST_TOKEN_PUNCTUATION,
    STRIKETHROUGH_OPEN,
    STRIKETHROUGH_CLOSE,
    LATEX_SPAN_START,
    LATEX_SPAN_CLOSE,
    UNCLOSED_SPAN
} TokenType;

// Determines if a character is punctuation as defined by the markdown spec.
static bool is_punctuation(int32_t chr) {
    return (chr >= '!' && chr <= '/') || (chr >= ':' && chr <= '@') ||
           (chr >= '[' && chr <= '`') || (chr >= '{' && chr <= '~');
}

// State bitflags used with `Scanner.state`

// Length modulo 3 of the last `*` delimiter run that opened emphasis, for the
// "rule of three" (CommonMark §6.2, rules 9 and 10)
static const uint8_t STATE_EMPHASIS_DELIMITER_MOD_3 = 0x3;
// Current delimiter run is opening
static const uint8_t STATE_EMPHASIS_DELIMITER_IS_OPEN = 0x1 << 2;
// Length modulo 3 of the last `_` delimiter run that opened emphasis
static const uint8_t STATE_UNDERSCORE_DELIMITER_MOD_3_SHIFT = 3;
static const uint8_t STATE_UNDERSCORE_DELIMITER_MOD_3 = 0x3 << 3;

// The rule of three (CommonMark §6.2, rules 9 and 10): "If one of the
// delimiters can both open and close emphasis, then the sum of the lengths of
// the delimiter runs containing the opening and closing delimiters must not be
// a multiple of 3 unless both lengths are multiples of 3." Only the lengths
// modulo 3 are needed for that.
static bool rule_of_three_forbids(uint8_t opener_mod_3, uint8_t closer_length) {
    uint8_t closer_mod_3 = closer_length % 3;
    return (opener_mod_3 + closer_mod_3) % 3 == 0 &&
           !(opener_mod_3 == 0 && closer_mod_3 == 0);
}

// Convenience function to emit the error token. This is done to stop invalid
// parse branches. Specifically:
// 1. When encountering a newline after a line break that ended a paragraph, and
// no new block
//    has been opened.
// 2. When encountering a new block after a soft line break.
// 3. When a `$._trigger_error` token is valid, which is used to stop parse
// branches through
//    normal tree-sitter grammar rules.
//
// See also the `$._soft_line_break` and `$._paragraph_end_newline` tokens in
// grammar.js
static bool error(TSLexer *lexer) {
    lexer->result_symbol = ERROR;
    return true;
}

typedef struct {
    // Parser state flags
    uint8_t state;
    uint8_t code_span_delimiter_length;
    uint8_t latex_span_delimiter_length;
    // The number of characters remaining in the currrent emphasis delimiter
    // run.
    uint8_t num_emphasis_delimiters_left;

} Scanner;

// Write the whole state of a Scanner to a byte buffer
static unsigned serialize(Scanner *s, char *buffer) {
    unsigned size = 0;
    buffer[size++] = (char)s->state;
    buffer[size++] = (char)s->code_span_delimiter_length;
    buffer[size++] = (char)s->latex_span_delimiter_length;
    buffer[size++] = (char)s->num_emphasis_delimiters_left;
    return size;
}

// Read the whole state of a Scanner from a byte buffer
// `serizalize` and `deserialize` should be fully symmetric.
static void deserialize(Scanner *s, const char *buffer, unsigned length) {
    s->state = 0;
    s->code_span_delimiter_length = 0;
    s->latex_span_delimiter_length = 0;
    s->num_emphasis_delimiters_left = 0;
    if (length > 0) {
        size_t size = 0;
        s->state = (uint8_t)buffer[size++];
        s->code_span_delimiter_length = (uint8_t)buffer[size++];
        s->latex_span_delimiter_length = (uint8_t)buffer[size++];
        s->num_emphasis_delimiters_left = (uint8_t)buffer[size++];
    }
}

static bool parse_leaf_delimiter(TSLexer *lexer, uint8_t *delimiter_length,
                                 const bool *valid_symbols,
                                 const char delimiter,
                                 const TokenType open_token,
                                 const TokenType close_token) {
    uint8_t level = 0;
    while (lexer->lookahead == delimiter) {
        lexer->advance(lexer, false);
        level++;
    }
    lexer->mark_end(lexer);
    if (level == *delimiter_length && valid_symbols[close_token]) {
        *delimiter_length = 0;
        lexer->result_symbol = close_token;
        return true;
    }
    if (valid_symbols[open_token]) {
        // Parse ahead to check if there is a closing delimiter
        size_t close_level = 0;
        while (!lexer->eof(lexer)) {
            if (lexer->lookahead == delimiter) {
                close_level++;
            } else {
                if (close_level == level) {
                    // Found a matching delimiter
                    break;
                }
                close_level = 0;
            }
            lexer->advance(lexer, false);
        }
        if (close_level == level) {
            *delimiter_length = level;
            lexer->result_symbol = open_token;
            return true;
        }
        if (valid_symbols[UNCLOSED_SPAN]) {
            lexer->result_symbol = UNCLOSED_SPAN;
            return true;
        }
    }
    return false;
}

static bool parse_backtick(Scanner *s, TSLexer *lexer,
                           const bool *valid_symbols) {
    return parse_leaf_delimiter(lexer, &s->code_span_delimiter_length,
                                valid_symbols, '`', CODE_SPAN_START,
                                CODE_SPAN_CLOSE);
}

static bool parse_dollar(Scanner *s, TSLexer *lexer,
                         const bool *valid_symbols) {
    return parse_leaf_delimiter(lexer, &s->latex_span_delimiter_length,
                                valid_symbols, '$', LATEX_SPAN_START,
                                LATEX_SPAN_CLOSE);
}

// How far `has_compatible_closer` looks ahead, in characters. Past it the
// scanner keeps its old behaviour, so the rule of three costs at most this much
// per delimiter run it applies to, and pathological input stays linear.
static const unsigned RULE_OF_THREE_LOOKAHEAD = 1000;

static bool is_whitespace_or_line_end(TSLexer *lexer) {
    return lexer->eof(lexer) || lexer->lookahead == ' ' ||
           lexer->lookahead == '\t' || lexer->lookahead == '\n' ||
           lexer->lookahead == '\r';
}

// Whether the next run of `delimiter` in the inline content could close an
// opener run of `opener_length` that can also close, as the rule of three
// allows. The lexer stands just after the opener run; the token's end has
// already been marked, so looking ahead does not consume anything.
//
// A run that the rule of three forbids to close opens instead only if the next
// run can close it. Otherwise opening would leave both it and the earlier
// opener unmatched, where CommonMark would pair the earlier opener with a later
// closer, which a scanner deciding one run at a time cannot do; so `**a****b**`
// keeps the parse it had before. Asking about any later run, rather than the
// next one, turns many runs into openers that never close, and the number of
// parse branches grows quickly (`**a****b ` repeated).
//
// Code spans are skipped and backslash escapes are not delimiters. Links and
// raw HTML are not modelled, as elsewhere in this scanner.
static bool has_compatible_closer(TSLexer *lexer, int32_t delimiter,
                                  uint8_t opener_length) {
    bool previous_whitespace = false;
    bool previous_punctuation = true;   // the opener run's last delimiter
    for (unsigned seen = 0; seen < RULE_OF_THREE_LOOKAHEAD && !lexer->eof(lexer);) {
        int32_t chr = lexer->lookahead;
        if (chr == '\\') {
            lexer->advance(lexer, false);
            seen++;
            if (!lexer->eof(lexer)) {
                previous_punctuation = is_punctuation(lexer->lookahead);
                previous_whitespace = false;
                lexer->advance(lexer, false);
                seen++;
            }
            continue;
        }
        if (chr == '`') {
            unsigned ticks = 0;
            while (lexer->lookahead == '`') {
                ticks++;
                lexer->advance(lexer, false);
                seen++;
            }
            // A code span's content is text; find where it ends, if it does.
            unsigned closing = 0;
            while (!lexer->eof(lexer) && seen < RULE_OF_THREE_LOOKAHEAD) {
                if (lexer->lookahead == '`') {
                    closing++;
                } else if (closing == ticks) {
                    break;
                } else {
                    closing = 0;
                }
                lexer->advance(lexer, false);
                seen++;
            }
            previous_punctuation = true;
            previous_whitespace = false;
            continue;
        }
        if (chr == delimiter) {
            uint8_t length = 0;
            while (lexer->lookahead == delimiter && length < UINT8_MAX) {
                length++;
                lexer->advance(lexer, false);
                seen++;
            }
            bool next_whitespace = is_whitespace_or_line_end(lexer);
            bool next_punctuation = !lexer->eof(lexer) && is_punctuation(lexer->lookahead);
            bool closes = !previous_whitespace &&
                          (!previous_punctuation || next_punctuation || next_whitespace);
            // Only the next run of the delimiter is asked. The opener can
            // close, so the rule applies to it whatever it is.
            return closes && !rule_of_three_forbids(opener_length % 3, length);
        }
        previous_whitespace = chr == ' ' || chr == '\t' || chr == '\n' || chr == '\r';
        previous_punctuation = is_punctuation(chr);
        lexer->advance(lexer, false);
        seen++;
    }
    return false;
}

static bool parse_star(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    lexer->advance(lexer, false);
    // If `num_emphasis_delimiters_left` is not zero then we already decided
    // that this should be part of an emphasis delimiter run, so interpret it as
    // such.
    if (s->num_emphasis_delimiters_left > 0) {
        // The `STATE_EMPHASIS_DELIMITER_IS_OPEN` state flag tells us wether it
        // should be open or close.
        if ((s->state & STATE_EMPHASIS_DELIMITER_IS_OPEN) &&
            valid_symbols[EMPHASIS_OPEN_STAR]) {
            s->state &= (~STATE_EMPHASIS_DELIMITER_IS_OPEN);
            lexer->result_symbol = EMPHASIS_OPEN_STAR;
            s->num_emphasis_delimiters_left--;
            return true;
        }
        if (valid_symbols[EMPHASIS_CLOSE_STAR]) {
            lexer->result_symbol = EMPHASIS_CLOSE_STAR;
            s->num_emphasis_delimiters_left--;
            return true;
        }
    }
    lexer->mark_end(lexer);
    // Otherwise count the number of stars
    uint8_t star_count = 1;
    while (lexer->lookahead == '*') {
        star_count++;
        lexer->advance(lexer, false);
    }
    bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                    lexer->eof(lexer);
    if (valid_symbols[EMPHASIS_OPEN_STAR] ||
        valid_symbols[EMPHASIS_CLOSE_STAR]) {
        // The desicion made for the first star also counts for all the
        // following stars in the delimiter run. Rembemer how many there are.
        s->num_emphasis_delimiters_left = star_count - 1;
        // Look ahead to the next symbol (after the last star) to find out if it
        // is whitespace punctuation or other.
        bool next_symbol_whitespace =
            line_end || lexer->lookahead == ' ' || lexer->lookahead == '\t';
        bool next_symbol_punctuation = is_punctuation(lexer->lookahead);
        // Information about the last token is in valid_symbols. See grammar.js
        // for these tokens for how this is done.
        bool can_close = valid_symbols[EMPHASIS_CLOSE_STAR] &&
                         !valid_symbols[LAST_TOKEN_WHITESPACE] &&
                         (!valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                          next_symbol_punctuation || next_symbol_whitespace);
        bool can_open = !next_symbol_whitespace &&
                        (!next_symbol_punctuation ||
                         valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                         valid_symbols[LAST_TOKEN_WHITESPACE]);
        // Closing delimiters take precedence, unless the run could also open,
        // the rule of three forbids it to close the last opener, and the next
        // run could close it: then it opens, as in `*foo**bar**baz*`.
        if (can_close &&
            !(can_open &&
              rule_of_three_forbids(s->state & STATE_EMPHASIS_DELIMITER_MOD_3,
                                    star_count) &&
              has_compatible_closer(lexer, '*', star_count))) {
            s->state &= ~STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = EMPHASIS_CLOSE_STAR;
            return true;
        }
        if (can_open) {
            s->state |= STATE_EMPHASIS_DELIMITER_IS_OPEN;
            s->state = (uint8_t)((s->state & ~STATE_EMPHASIS_DELIMITER_MOD_3) |
                                 (star_count % 3));
            lexer->result_symbol = EMPHASIS_OPEN_STAR;
            return true;
        }
    }
    return false;
}

static bool parse_tilde(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    lexer->advance(lexer, false);
    // If `num_emphasis_delimiters_left` is not zero then we already decided
    // that this should be part of an emphasis delimiter run, so interpret it as
    // such.
    if (s->num_emphasis_delimiters_left > 0) {
        // The `STATE_EMPHASIS_DELIMITER_IS_OPEN` state flag tells us wether it
        // should be open or close.
        if ((s->state & STATE_EMPHASIS_DELIMITER_IS_OPEN) &&
            valid_symbols[STRIKETHROUGH_OPEN]) {
            s->state &= (~STATE_EMPHASIS_DELIMITER_IS_OPEN);
            lexer->result_symbol = STRIKETHROUGH_OPEN;
            s->num_emphasis_delimiters_left--;
            return true;
        }
        if (valid_symbols[STRIKETHROUGH_CLOSE]) {
            lexer->result_symbol = STRIKETHROUGH_CLOSE;
            s->num_emphasis_delimiters_left--;
            return true;
        }
    }
    lexer->mark_end(lexer);
    // Otherwise count the number of tildes
    uint8_t star_count = 1;
    while (lexer->lookahead == '~') {
        star_count++;
        lexer->advance(lexer, false);
    }
    bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                    lexer->eof(lexer);
    if (valid_symbols[STRIKETHROUGH_OPEN] ||
        valid_symbols[STRIKETHROUGH_CLOSE]) {
        // The desicion made for the first star also counts for all the
        // following stars in the delimiter run. Rembemer how many there are.
        s->num_emphasis_delimiters_left = star_count - 1;
        // Look ahead to the next symbol (after the last star) to find out if it
        // is whitespace punctuation or other.
        bool next_symbol_whitespace =
            line_end || lexer->lookahead == ' ' || lexer->lookahead == '\t';
        bool next_symbol_punctuation = is_punctuation(lexer->lookahead);
        // Information about the last token is in valid_symbols. See grammar.js
        // for these tokens for how this is done.
        if (valid_symbols[STRIKETHROUGH_CLOSE] &&
            !valid_symbols[LAST_TOKEN_WHITESPACE] &&
            (!valid_symbols[LAST_TOKEN_PUNCTUATION] ||
             next_symbol_punctuation || next_symbol_whitespace)) {
            // Closing delimiters take precedence
            s->state &= ~STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = STRIKETHROUGH_CLOSE;
            return true;
        }
        if (!next_symbol_whitespace && (!next_symbol_punctuation ||
                                        valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                                        valid_symbols[LAST_TOKEN_WHITESPACE])) {
            s->state |= STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = STRIKETHROUGH_OPEN;
            return true;
        }
    }
    return false;
}

static bool parse_underscore(Scanner *s, TSLexer *lexer,
                             const bool *valid_symbols) {
    lexer->advance(lexer, false);
    // If `num_emphasis_delimiters_left` is not zero then we already decided
    // that this should be part of an emphasis delimiter run, so interpret it as
    // such.
    if (s->num_emphasis_delimiters_left > 0) {
        // The `STATE_EMPHASIS_DELIMITER_IS_OPEN` state flag tells us wether it
        // should be open or close.
        if ((s->state & STATE_EMPHASIS_DELIMITER_IS_OPEN) &&
            valid_symbols[EMPHASIS_OPEN_UNDERSCORE]) {
            s->state &= (~STATE_EMPHASIS_DELIMITER_IS_OPEN);
            lexer->result_symbol = EMPHASIS_OPEN_UNDERSCORE;
            s->num_emphasis_delimiters_left--;
            return true;
        }
        if (valid_symbols[EMPHASIS_CLOSE_UNDERSCORE]) {
            lexer->result_symbol = EMPHASIS_CLOSE_UNDERSCORE;
            s->num_emphasis_delimiters_left--;
            return true;
        }
    }
    lexer->mark_end(lexer);
    // Otherwise count the number of stars
    uint8_t underscore_count = 1;
    while (lexer->lookahead == '_') {
        underscore_count++;
        lexer->advance(lexer, false);
    }
    bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                    lexer->eof(lexer);
    if (valid_symbols[EMPHASIS_OPEN_UNDERSCORE] ||
        valid_symbols[EMPHASIS_CLOSE_UNDERSCORE]) {
        // The desicion made for the first underscore also counts for all the
        // following underscores in the delimiter run. Rembemer how many there are.
        s->num_emphasis_delimiters_left = underscore_count - 1;
        // Look ahead to the next symbol (after the last underscore) to find out if it
        // is whitespace punctuation or other.
        bool next_symbol_whitespace =
            line_end || lexer->lookahead == ' ' || lexer->lookahead == '\t';
        bool next_symbol_punctuation = is_punctuation(lexer->lookahead);
        // Information about the last token is in valid_symbols. See grammar.js
        // for these tokens for how this is done.
        bool can_close = valid_symbols[EMPHASIS_CLOSE_UNDERSCORE] &&
                         !valid_symbols[LAST_TOKEN_WHITESPACE] &&
                         (!valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                          next_symbol_punctuation || next_symbol_whitespace);
        bool can_open = !next_symbol_whitespace &&
                        (!next_symbol_punctuation ||
                         valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                         valid_symbols[LAST_TOKEN_WHITESPACE]);
        // As for `*`: closing takes precedence unless the rule of three
        // forbids it and the run could open instead.
        uint8_t opener_mod_3 =
            (s->state & STATE_UNDERSCORE_DELIMITER_MOD_3) >>
            STATE_UNDERSCORE_DELIMITER_MOD_3_SHIFT;
        if (can_close &&
            !(can_open && rule_of_three_forbids(opener_mod_3, underscore_count) &&
              has_compatible_closer(lexer, '_', underscore_count))) {
            s->state &= ~STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = EMPHASIS_CLOSE_UNDERSCORE;
            return true;
        }
        if (can_open) {
            s->state |= STATE_EMPHASIS_DELIMITER_IS_OPEN;
            s->state = (uint8_t)((s->state & ~STATE_UNDERSCORE_DELIMITER_MOD_3) |
                                 ((underscore_count % 3)
                                  << STATE_UNDERSCORE_DELIMITER_MOD_3_SHIFT));
            lexer->result_symbol = EMPHASIS_OPEN_UNDERSCORE;
            return true;
        }
    }
    return false;
}

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    // A normal tree-sitter rule decided that the current branch is invalid and
    // now "requests" an error to stop the branch
    if (valid_symbols[TRIGGER_ERROR]) {
        return error(lexer);
    }

    // Decide which tokens to consider based on the first non-whitespace
    // character
    switch (lexer->lookahead) {
        case '`':
            // A backtick could mark the beginning or ending of a code span or a
            // fenced code block.
            return parse_backtick(s, lexer, valid_symbols);
        case '$':
            return parse_dollar(s, lexer, valid_symbols);
        case '*':
            // A star could either mark the beginning or ending of emphasis, a
            // list item or thematic break. This code is similar to the code for
            // '_' and '+'.
            return parse_star(s, lexer, valid_symbols);
        case '_':
            return parse_underscore(s, lexer, valid_symbols);
        case '~':
            return parse_tilde(s, lexer, valid_symbols);
    }
    return false;
}

void *tree_sitter_markdown_inline_external_scanner_create() {
    Scanner *s = (Scanner *)malloc(sizeof(Scanner));
    deserialize(s, NULL, 0);
    return s;
}

bool tree_sitter_markdown_inline_external_scanner_scan(
    void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    return scan(scanner, lexer, valid_symbols);
}

unsigned tree_sitter_markdown_inline_external_scanner_serialize(void *payload,
                                                                char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    return serialize(scanner, buffer);
}

void tree_sitter_markdown_inline_external_scanner_deserialize(void *payload,
                                                              const char *buffer,
                                                              unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    deserialize(scanner, buffer, length);
}

void tree_sitter_markdown_inline_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    free(scanner);
}
