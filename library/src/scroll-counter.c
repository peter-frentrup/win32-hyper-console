#include <hyper-console.h>

#include "scroll-counter.h"

#include "console-buffer-io.h"
#include "memory-util.h"

#include <assert.h>
#include <limits.h>


#define MIN(A, B)  ((A) < (B) ? (A) : (B))

/* Due to line (un-)wrapping, global lines can map to screen lines in the
   following ways.

       Global     G->1   G->2    Local1  1->G      Loc2  2->G
   11  XXXXX      0:0    0:0     XXXXXY  11:0      XXXX  11:0
   12  YYY        0:5    1:1     YY....  12:1      XYYY  11:4
   13  ZZZZZZ     2:0    2:0     ZZZZZZ  13:0      ZZZZ  13:0
   14  WW         3:0    4:0     WW....  14:0      ZZZ.  13:4
                                                   WW..  14:0
 */


struct text_line_t {
  int length;
  wchar_t content[1];
};

struct global_coord_t {
  int column;
  int line;
};

struct text_array_t {
  int count;
  int capacity;
  struct text_line_t **lines;
};

struct line_numbers_array_t {
  int count;
  int capacity;
  struct global_coord_t *line_starts;
};

struct console_scrollback_t {
  HANDLE output_handle;
  
  struct text_array_t         old_lines;
  struct line_numbers_array_t visible_line_numbers;
  
  int past_lines_count;
};

static struct text_line_t *create_text_line(int length);
static void free_text_line(struct text_line_t *line);

static void remove_lines_front(struct text_array_t *text, int num_remove);
static void clear_lines(struct text_array_t *text);
static BOOL append_null_lines(struct text_array_t *text, int num_add);

static BOOL resize_line_numbers_array(struct line_numbers_array_t *array, int count);
static void clear_line_numbers_array(struct line_numbers_array_t *array);

static BOOL is_space_only(const wchar_t *str, int length);
static int is_match(const struct text_line_t *original, const wchar_t *visible, COORD visible_size);
static int apply_match_at(const struct text_array_t *original, int orig_start, int past_lines_count, struct global_coord_t *visible_coords, const wchar_t *visible, COORD visible_size);
static void match_lines(struct console_scrollback_t *cs, const wchar_t *visible, COORD visible_size);
static void clean_old_lines(struct console_scrollback_t *cs);
static void append_new_known_lines(struct console_scrollback_t *cs, const wchar_t *visible, COORD visible_size);

/** Create a new uninitialized text-line.
 */
static struct text_line_t *create_text_line(int length) {
  struct text_line_t *result;
  size_t size;
  
  assert(length >= 0);
  
  size = sizeof(wchar_t) * (size_t)length + sizeof(struct text_line_t);
  result = hyper_console_allocate_memory(size);
  if(!result)
    return NULL;
    
  result->content[length] = L'\0';
  result->length = length;
  return result;
}

static void free_text_line(struct text_line_t *line) {
  hyper_console_free_memory(line);
}


static void remove_lines_front(struct text_array_t *text, int num_remove) {
  int i;
  
  assert(text != NULL);
  assert(num_remove >= 0);
  assert(num_remove <= text->count);
  
  for(i = 0; i < num_remove; ++i)
    free_text_line(text->lines[i]);
    
  memmove(
    text->lines,
    text->lines + num_remove,
    (text->count - num_remove) * sizeof(struct text_line_t*));
    
  text->count -= num_remove;
}

static void clear_lines(struct text_array_t *text) {
  int i;
  
  assert(text != NULL);
  
  for(i = 0; i < text->count; ++i)
    free_text_line(text->lines[i]);
    
  hyper_console_free_memory(text->lines);
  memset(text, 0, sizeof(*text));
}

static BOOL append_null_lines(struct text_array_t *text, int num_add) {

  assert(text != NULL);
  assert(num_add >= 0);
  
  if(num_add > INT_MAX - text->count)
    return FALSE;
    
  if(!resize_array(
        (void**)&text->lines,
        &text->capacity,
        sizeof(struct text_line_t*),
        text->count + num_add))
  {
    return FALSE;
  }
  
  memset(text->lines + text->count, 0, num_add * sizeof(struct text_line_t*));
  text->count += num_add;
  return TRUE;
}


static BOOL resize_line_numbers_array(struct line_numbers_array_t *array, int count) {

  assert(array != NULL);
  assert(count >= 0);
  
  if(resize_array(
        (void**)&array->line_starts,
        &array->capacity,
        sizeof(array->line_starts[0]),
        count))
  {
    array->count = count;
    
    return TRUE;
  }
  
  return FALSE;
}

static void clear_line_numbers_array(struct line_numbers_array_t *array)  {

  assert(array != NULL);
  
  hyper_console_free_memory(array->line_starts);
  
  memset(array, 0, sizeof(*array));
}

/** Check whether a string consists only of space characters.
 */
static BOOL is_space_only(const wchar_t *str, int length) {
  assert(length >= 0);
  
  for(; length > 0; --length, ++str) {
    if(*str != L' ')
      return FALSE;
  }
  
  return TRUE;
}

/** Return the number of visible lines that match the original line (Could be > 1 in case of line-wrapping).
   @param original     The long line to be matched.
   @param visible      The visible lines which should start @a original.
   @param visible_size The line length (X) and maximum number of visible lines (Y).

   @return The number of matching visible lines. That is, after space-extending @a original, it equals
   visible[0..<return>*visible_size.X].
 */
static int is_match(const struct text_line_t *original, const wchar_t *visible, COORD visible_size) {
  int matched_lines;
  int orig_rest_length;
  const wchar_t *orig_rest;
  
  assert(original != NULL);
  assert(visible != NULL);
  assert(visible_size.X > 0);
  assert(visible_size.Y >= 0);
  
  orig_rest = original->content;
  orig_rest_length = original->length;
  
  matched_lines = 0;
  while(visible_size.X <= orig_rest_length && visible_size.Y > 0) {
    if(0 != memcmp(orig_rest, visible, visible_size.X * sizeof(wchar_t)))
      return matched_lines;
      
    ++matched_lines;
    --visible_size.Y;
    visible += visible_size.X;
    orig_rest += visible_size.X;
    orig_rest_length -= visible_size.X;
  }
  
  if(visible_size.Y == 0)
    return matched_lines;
    
  if(0 != memcmp(orig_rest, visible, orig_rest_length * sizeof(wchar_t)))
    return matched_lines;
    
  if(!is_space_only(visible + orig_rest_length, visible_size.X - orig_rest_length))
    return matched_lines;
    
  ++matched_lines;
  return matched_lines;
}

/** Match visible lines and store their detected global coordinates.
  @param original         The array of original lines to be matched.
  @param orig_start       Index of the @a original line to match the first @a visible line.
  @param past_lines_count Offset to add to the matched line index to get the global line number.
  @param visible_coords   [out] Coordinates for the visible lines. Must be an array of length @a visible_size.Y.
  @param visible          The visible lines.
  @param visible_size     The line length (X) and maximum number of visible lines (Y).

  @return The number of visible lines matched.
 */
static int apply_match_at(
  const struct text_array_t *original,
  int orig_start,
  int past_lines_count,
  struct global_coord_t *visible_coords,
  const wchar_t *visible,
  COORD visible_size
) {
  int visible_matched_lines = 0;
  
  while(orig_start < original->count && visible_size.Y > 0) {
    int i;
    
    int num_vis = is_match(original->lines[orig_start], visible, visible_size);
    if(num_vis == 0)
      break;
      
    for(i = 0; i < num_vis; ++i, ++visible_coords) {
      visible_coords->line = orig_start + past_lines_count;
      visible_coords->column = i * visible_size.X;
    }
    
    visible_matched_lines += num_vis;
    visible += num_vis * visible_size.X;
    visible_size.Y -= (SHORT)num_vis;
    ++orig_start;
  }
  
  return visible_matched_lines;
}

/** Estimate the global line numbers of the top-most visible lines.
 */
static void match_lines(struct console_scrollback_t *cs, const wchar_t *visible, COORD visible_size) {
  int first;
  
  assert(visible_size.X > 0);
  assert(visible_size.Y > 0);
  assert(cs != NULL);
  assert(visible != NULL);
  
  if(!resize_line_numbers_array(&cs->visible_line_numbers, visible_size.Y)) {
    cs->visible_line_numbers.count = 0;
    return;
  }
  
  for(first = 0; first < cs->old_lines.count; ++first) {
    struct global_coord_t *vis_coords = cs->visible_line_numbers.line_starts;
    
    int vis_match = apply_match_at(
                      &cs->old_lines,
                      first,
                      cs->past_lines_count,
                      cs->visible_line_numbers.line_starts,
                      visible,
                      visible_size);
                      
    assert(vis_match <= visible_size.Y);
    
    if(vis_match > 0) {
      int last_matched_global_line_index = vis_coords[vis_match - 1].line - cs->past_lines_count;
      
      assert(last_matched_global_line_index <= cs->old_lines.count);
      
      if(last_matched_global_line_index + 1 == cs->old_lines.count) {
        /* Success, found whole rest of cs->old_lines */
        cs->visible_line_numbers.count = vis_match;
        
        return;
      }
    }
  }
  
  /* No match found.
    TODO: the first visible lines might be the rest of some old lines due to line-breaking.
   */
  cs->visible_line_numbers.count = 0;
}

/** Remove the (initial) lines from the old_lines cache, which do not match the currently visible lines according to visible_line_numbers.
 */
static void clean_old_lines(struct console_scrollback_t *cs) {
  int remove_count;
  
  assert(cs != NULL);
  
  if(cs->visible_line_numbers.count == 0) {
    cs->past_lines_count += cs->old_lines.count;
    
    clear_lines(&cs->old_lines);
    return;
  }
  
  remove_count = cs->visible_line_numbers.line_starts[0].line - cs->past_lines_count;
  assert(remove_count >= 0);
  
  cs->past_lines_count += remove_count;
  remove_lines_front(&cs->old_lines, remove_count);
}

/** Append those @a visible lines to the old_lines cache, which are not listed there yet (according to visible_line_numbers).
 */
static void append_new_known_lines(struct console_scrollback_t *cs, const wchar_t *visible, COORD visible_size) {
  int new_count;
  int old_lines_offset;
  int visible_lines_offset;
  
  assert(cs != NULL);
  assert(visible != NULL);
  assert(visible_size.X > 0);
  assert(visible_size.Y >= cs->visible_line_numbers.count);
  
  old_lines_offset     = cs->old_lines.count;
  visible_lines_offset = cs->visible_line_numbers.count;
  
  new_count = visible_size.Y - visible_lines_offset;
  visible += visible_lines_offset * visible_size.X;
  
  if(append_null_lines(&cs->old_lines, new_count)) {
    int i;
    
    if(!resize_line_numbers_array(&cs->visible_line_numbers, cs->visible_line_numbers.count + new_count)) {
      cs->old_lines.count = old_lines_offset;
      return;
    }
    
    for(i = 0; i < new_count; ++i, visible += visible_size.X) {
      struct text_line_t *line = create_text_line(visible_size.X);
      
      if(line == NULL) {
        cs->old_lines.count = i;
        break;
      }
      
      memcpy(line->content, visible, visible_size.X * sizeof(wchar_t));
      while(line->length > 0 && line->content[line->length - 1] == ' ')
        line->length--;
      line->content[line->length] = '\0';
      
      cs->old_lines.lines[old_lines_offset + i] = line;
      cs->visible_line_numbers.line_starts[visible_lines_offset + i].column = 0;
      cs->visible_line_numbers.line_starts[visible_lines_offset + i].line = cs->past_lines_count + old_lines_offset + i;
    }
  }
}

struct console_scrollback_t *console_scrollback_new(void) {
  struct console_scrollback_t *cs;
  
  cs = hyper_console_allocate_memory(sizeof(struct console_scrollback_t));
  if(cs) {
    memset(cs, 0, sizeof(*cs));
    cs->output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  }
  
  return cs;
}

void console_scrollback_free(struct console_scrollback_t *cs) {
  if(!cs)
    return;
    
  clear_lines(&cs->old_lines);
  clear_line_numbers_array(&cs->visible_line_numbers);
  hyper_console_free_memory(cs);
}

void console_scrollback_update(struct console_scrollback_t *cs, int known_visible_lines) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  CHAR_INFO *visible_cells;
  COORD visible_size;
  
  if(cs == NULL)
    return;
    
  if(!GetConsoleScreenBufferInfo(cs->output_handle, &csbi)) {
    return;
  }
  
  if(known_visible_lines < 0)
    known_visible_lines = 0;
    
  if(known_visible_lines > csbi.dwSize.Y)
    known_visible_lines = csbi.dwSize.Y;
    
  visible_size.X = csbi.dwSize.X;
  visible_size.Y = (SHORT)known_visible_lines;
  
  if(visible_size.X <= 0 || visible_size.Y <= 0) {
    clear_line_numbers_array(&cs->visible_line_numbers);
    return;
  }
  
  visible_cells = hyper_console_allocate_memory(visible_size.X * visible_size.Y * sizeof(CHAR_INFO));
  if(visible_cells != NULL) {
    SMALL_RECT read_region;
    read_region.Left = 0;
    read_region.Top = 0;
    read_region.Right = read_region.Left + visible_size.X - 1;
    read_region.Bottom = read_region.Top + visible_size.Y - 1;
    COORD write_pos;
    write_pos.X = 0;
    write_pos.Y = 0;
    if( console_read_output(
          cs->output_handle,
          visible_cells,
          visible_size,
          write_pos,
          &read_region) &&
       read_region.Right + 1 - read_region.Left == visible_size.X &&
       read_region.Bottom + 1 - read_region.Top == visible_size.Y) 
    {
      wchar_t *visible_lines = (wchar_t*)visible_cells;
      for(int i = 0;i < visible_size.X * visible_size.Y;++i)
        visible_lines[i] = visible_cells[i].Char.UnicodeChar;
      
      match_lines(cs, visible_lines, visible_size);
      clean_old_lines(cs);
      append_new_known_lines(cs, visible_lines, visible_size);
    }
    
    hyper_console_free_memory(visible_cells);
  }
}

BOOL console_scollback_local_to_global(struct console_scrollback_t *cs, COORD local, int *line, int *column) {
  const struct global_coord_t *coords;
  int vis_y_count;
  
  assert(line != NULL);
  assert(column != NULL);
  
  *line = 0;
  *column = 0;
  
  if(cs == NULL)
    return FALSE;
    
  vis_y_count = cs->visible_line_numbers.count;
  
  if(local.Y < 0 || local.Y > vis_y_count)
    return FALSE;
    
  if(vis_y_count == 0) {
    *line = cs->past_lines_count;
    *column = local.X;
    return TRUE;
  }
  
  coords = cs->visible_line_numbers.line_starts;
  
  if(local.Y == vis_y_count) {
    *line = coords[vis_y_count - 1].line + 1;
    *column = local.X;
    return TRUE;
  }
  
  *line = coords[local.Y].line;
  *column = local.X + coords[local.Y].column;
  
  return TRUE;
}

BOOL console_scollback_global_to_local(struct console_scrollback_t *cs, int line, int column, COORD *local) {
  const struct global_coord_t *coords;
  int vis_y;
  int vis_y_count;
  int next_global_line;
  
  assert(local != NULL);
  assert(column >= 0);
  
  local->X = 0;
  local->Y = 0;
  
  if(cs == NULL)
    return FALSE;
    
  vis_y_count = cs->visible_line_numbers.count;
  
  if(line < cs->past_lines_count)
    return FALSE;
    
  coords = cs->visible_line_numbers.line_starts;
  
  if(vis_y_count > 0) {
    next_global_line = coords[vis_y_count - 1].line + 1;
  }
  else
    next_global_line = cs->past_lines_count;
    
  if(line == next_global_line) {
    local->Y = (SHORT)vis_y_count;
    local->X = (SHORT)MIN(column, SHRT_MAX);
    return TRUE;
  }
  
  if(line > next_global_line)
    return FALSE;
    
  for(vis_y = line - cs->past_lines_count; vis_y < vis_y_count; ++vis_y) {
    if(coords[vis_y].line == line) {
      int break_column = INT_MAX;
      
      if( vis_y + 1 < vis_y_count && coords[vis_y + 1].line == line) {
        break_column = coords[vis_y + 1].column;
      }
      
      if(column < break_column) {
        local->Y = (SHORT)vis_y;
        local->X = (SHORT)(column - coords[vis_y].column);
        return TRUE;
      }
    }
    
    if(line < coords[vis_y].line)
      break;
  }
  
  return FALSE;
}
