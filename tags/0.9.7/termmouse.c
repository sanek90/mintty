// termmouse.c (part of mintty)
// Copyright 2008-10 Andy Koppe
// Based on code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "termpriv.h"
#include "win.h"
#include "child.h"

/*
 * Fetch the character at a particular position in a line array.
 * The reason this isn't just a simple array reference is that if the
 * character we find is UCSWIDE, then we must look one space further
 * to the left.
 */
static wchar
get_char(termline *line, int x)
{
  wchar c = line->chars[x].chr;
  if (c == UCSWIDE && x > 0)
    c = line->chars[x - 1].chr;
  return c;
}

static pos
sel_spread_word(pos p, bool forward)
{
  pos ret_p = p;
  termline *line = fetch_line(p.y);
  
  for (;;) {
    wchar c = get_char(line, p.x);
    if (iswalnum(c))
      ret_p = p;
    else if (term.mouse_state != MS_OPENING && *cfg.word_chars) {
      if (!strchr(cfg.word_chars, c))
        break;
      ret_p = p;
    }
    else if (strchr("_#~+-", c))
      ret_p = p;
    else if (strchr(".@/\\", c)) {
      if (!forward)
        ret_p = p;
    }
    else if (!(strchr("&,?$%", c) || c == (forward ? '=' : ':')))
      break;

    if (forward) {
      p.x++;
      if (p.x >= term.cols - ((line->attr & LATTR_WRAPPED2) != 0)) {
        if (!(line->attr & LATTR_WRAPPED))
          break;
        p.x = 0;
        release_line(line);
        line = fetch_line(++p.y);
      }
    }
    else {
      if (p.x <= 0) {
        if (p.y <= -sblines())
          break;
        release_line(line);
        line = fetch_line(--p.y);
        if (!(line->attr & LATTR_WRAPPED))
          break;
        p.x = term.cols - ((line->attr & LATTR_WRAPPED2) != 0);
      }
      p.x--;
    }
  }
    
  release_line(line);
  return ret_p;
}

/*
 * Spread the selection outwards according to the selection mode.
 */
static pos
sel_spread_half(pos p, bool forward)
{
  switch (term.mouse_state) {
    when MS_SEL_CHAR: {
     /*
      * In this mode, every character is a separate unit, except
      * for runs of spaces at the end of a non-wrapping line.
      */
      termline *line = fetch_line(p.y);
      if (!(line->attr & LATTR_WRAPPED)) {
        termchar *q = line->chars + term.cols;
        while (q > line->chars && q[-1].chr == ' ' && !q[-1].cc_next)
          q--;
        if (q == line->chars + term.cols)
          q--;
        if (p.x >= q - line->chars)
          p.x = forward ? term.cols - 1 : q - line->chars;
      }
      release_line(line);
    }
    when MS_SEL_WORD or MS_OPENING:
      p = sel_spread_word(p, forward); 
    when MS_SEL_LINE:
      if (forward) {
        termline *line = fetch_line(p.y);
        while (line->attr & LATTR_WRAPPED) {
          release_line(line);
          line = fetch_line(++p.y);
          p.x = 0;
        }
        int x = p.x;
        p.x = term.cols - 1;
        do {
          if (get_char(line, x) != ' ')
            p.x = x;
        } while (++x < line->cols);
        release_line(line);
      }
      else {
        p.x = 0;
        while (p.y > -sblines()) {
          termline *line = fetch_line(p.y - 1);
          bool wrapped = line->attr & LATTR_WRAPPED;
          release_line(line);
          if (!wrapped)
            break;
          p.y--;
        }
      }
    default:
     /* Shouldn't happen. */
      break;
  }
  return p;
}

static void
sel_spread(void)
{
  term.sel_start = sel_spread_half(term.sel_start, false);
  term.sel_end = sel_spread_half(term.sel_end, true);
  incpos(term.sel_end);
}

static void
sel_drag(pos selpoint)
{
  term.selected = true;
  if (!term.sel_rect) {
   /*
    * For normal selection, we set (sel_start,sel_end) to
    * (selpoint,sel_anchor) in some order.
    */
    if (poslt(selpoint, term.sel_anchor)) {
      term.sel_start = selpoint;
      term.sel_end = term.sel_anchor;
    }
    else {
      term.sel_start = term.sel_anchor;
      term.sel_end = selpoint;
    }
    sel_spread();
  }
  else {
   /*
    * For rectangular selection, we may need to
    * interchange x and y coordinates (if the user has
    * dragged in the -x and +y directions, or vice versa).
    */
    term.sel_start.x = min(term.sel_anchor.x, selpoint.x);
    term.sel_end.x = 1 + max(term.sel_anchor.x, selpoint.x);
    term.sel_start.y = min(term.sel_anchor.y, selpoint.y);
    term.sel_end.y = max(term.sel_anchor.y, selpoint.y);
  }
}

static void
sel_extend(pos selpoint)
{
  if (term.selected) {
    if (!term.sel_rect) {
     /*
      * For normal selection, we extend by moving
      * whichever end of the current selection is closer
      * to the mouse.
      */
      if (posdiff(selpoint, term.sel_start) <
          posdiff(term.sel_end, term.sel_start) / 2) {
        term.sel_anchor = term.sel_end;
        decpos(term.sel_anchor);
      }
      else
        term.sel_anchor = term.sel_start;
    }
    else {
     /*
      * For rectangular selection, we have a choice of
      * _four_ places to put sel_anchor and selpoint: the
      * four corners of the selection.
      */
      term.sel_anchor.x = 
        selpoint.x * 2 < term.sel_start.x + term.sel_end.x
        ? term.sel_end.x - 1
        : term.sel_start.x;
      term.sel_anchor.y = 
        selpoint.y * 2 < term.sel_start.y + term.sel_end.y
        ? term.sel_end.y
        : term.sel_start.y;
    }
  }
  else
    term.sel_anchor = selpoint;
  sel_drag(selpoint);
}

static void
send_mouse_event(char code, mod_keys mods, pos p)
{
  char buf[8] = "\e[M";
  uint len = 3;
  buf[len++] = code | (mods & ~cfg.click_target_mod) << 2;
  
  void encode_coord(int c) {
    c += ' ' + 1;
    if (!term.ext_mouse_pos)
      buf[len++] = c < 0x100 ? c : 0; 
    else if (c < 0x80)
      buf[len++] = c;
    else if (c < 0x800) {
      // In extended mouse mode, positions from 96 to 2015 are encoded as a
      // two-byte UTF-8 sequence (as introduced in xterm #262.)
      buf[len++] = 0xC0 + (c >> 6);
      buf[len++] = 0x80 + (c & 0x3F);
    }
    else {
      // Xterm reports out-of-range positions as a NUL byte.
      buf[len++] = 0;
    }
  }
  
  encode_coord(p.x);
  encode_coord(p.y);

  child_write(buf, len);
}

static pos
box_pos(pos p)
{
  p.y = min(max(0, p.y), term.rows - 1);
  p.x = min(max(0, p.x), term.cols - 1);
  return p;
}

static pos
get_selpoint(const pos p)
{
  pos sp = { .y = p.y + term.disptop, .x = p.x };
  termline *line = fetch_line(sp.y);
  if ((line->attr & LATTR_MODE) != LATTR_NORM)
    sp.x /= 2;

 /*
  * Transform x through the bidi algorithm to find the _logical_
  * click point from the physical one.
  */
  if (term_bidi_line(line, p.y) != null)
    sp.x = term.post_bidi_cache[p.y].backward[sp.x];
  
  // Back to previous cell if current one is second half of a wide char
  if (line->chars[sp.x].chr == UCSWIDE)
    sp.x--;
  
  release_line(line);
  return sp;
}

static void
send_keys(char *code, uint len, uint count)
{
  if (count) {
    uint size = len * count;
    char buf[size], *p = buf;
    while (count--) { memcpy(p, code, len); p += len; }
    child_write(buf, size);
  }
}

static bool
is_app_mouse(mod_keys *mods_p)
{
  if (!term.mouse_mode)
    return false;
  bool override = *mods_p & cfg.click_target_mod;
  *mods_p &= ~cfg.click_target_mod;
  return cfg.clicks_target_app ^ override;
}

void
term_mouse_click(mouse_button b, mod_keys mods, pos p, int count)
{
  if (!term.show_other_screen && is_app_mouse(&mods)) {
    if (term.mouse_mode == MM_X10)
      mods = 0;
    send_mouse_event(0x1F + b, mods, box_pos(p));
    term.mouse_state = b;
  }
  else {  
    bool alt = mods & MDK_ALT;
    bool shift_ctrl = mods & (MDK_SHIFT | MDK_CTRL);
    int rca = cfg.right_click_action;
    if (b == MBT_RIGHT && (rca == RC_MENU || shift_ctrl)) {
      if (!alt) 
        win_popup_menu();
    }
    else if (b == ((rca == RC_PASTE) ? MBT_RIGHT : MBT_MIDDLE)) {
      if (!alt) {
        if (shift_ctrl)
          term_copy();
        else
          win_paste();
      }
    }
    else if (b == MBT_LEFT && mods == MDK_CTRL) {
      // Open word under cursor
      p = get_selpoint(box_pos(p));
      term.mouse_state = MS_OPENING;
      term.selected = true;
      term.sel_rect = false;
      term.sel_start = term.sel_end = term.sel_anchor = p;
      sel_spread();
      win_update();
    }
    else {
      // Only clicks for selecting and extending should get here.
      p = get_selpoint(box_pos(p));
      term.mouse_state = -count;
      term.sel_rect = alt;
      if (b != MBT_LEFT || shift_ctrl)
        sel_extend(p);
      else if (count == 1) {
        term.selected = false;
        term.sel_anchor = p;
      }
      else {
        // Double or triple-click: select whole word or line
        term.selected = true;
        term.sel_rect = false;
        term.sel_start = term.sel_end = term.sel_anchor = p;
        sel_spread();
      }
      win_capture_mouse();
      win_update();
    }
  }
}

void
term_mouse_release(mod_keys mods, pos p)
{
  p = box_pos(p);
  int state = term.mouse_state;
  term.mouse_state = 0;
  if (state >= 0) {
    if (term.mouse_mode >= MM_VT200)
      send_mouse_event(0x23, mods, p);
  }
  else if (state == MS_OPENING) {
    term_open();
    term.selected = false;
    win_update();
  }
  else {
    // Finish selection.
    if (term.selected && cfg.copy_on_select)
      term_copy();
    
    // Flush any output held back during selection.
    term_flush();
    
    // "Clicks place cursor" implementation.
    if (!cfg.clicks_place_cursor || term.on_alt_screen || term.app_cursor_keys)
      return;
    
    pos dest = term.selected ? term.sel_end : get_selpoint(p);
    
    static bool moved_previously;
    static pos last_dest;
    
    pos orig;
    if (state == MS_SEL_CHAR)
      orig = (pos){.y = term.screen.curs.y, .x = term.screen.curs.x};
    else if (moved_previously)
      orig = last_dest;
    else
      return;
    
    bool forward = posle(orig, dest);
    pos end = forward ? dest : orig;
    p = forward ? orig : dest;
    
    uint count = 0;
    while (p.y != end.y) {
      termline *line = fetch_line(p.y);
      if (!(line->attr & LATTR_WRAPPED)) {
        release_line(line);
        moved_previously = false;
        return;
      }
      int cols = term.cols - ((line->attr & LATTR_WRAPPED2) != 0);
      for (int x = p.x; x < cols; x++) {
        if (line->chars[x].chr != UCSWIDE)
          count++;
      }
      p.y++;
      p.x = 0;
      release_line(line);
    }
    termline *line = fetch_line(p.y);
    for (int x = p.x; x < end.x; x++) {
      if (line->chars[x].chr != UCSWIDE)
        count++;
    }
    release_line(line);
    
    send_keys(forward ? "\e[C" : "\e[D", 3, count);
    
    moved_previously = true;
    last_dest = dest;
  }
}

static void
sel_scroll_cb(void)
{
  if (term_selecting() && term.sel_scroll) {
    term_scroll(0, term.sel_scroll);
    sel_drag(get_selpoint(term.sel_pos));
    win_update();
    win_set_timer(sel_scroll_cb, 125);
  }
}

void
term_mouse_move(mod_keys mods, pos p)
{
  pos bp = box_pos(p);
  if (term_selecting()) {
    if (p.y < 0 || p.y >= term.rows) {
      if (!term.sel_scroll) 
        win_set_timer(sel_scroll_cb, 200);
      term.sel_scroll = p.y < 0 ? p.y : p.y - term.rows + 1;
      term.sel_pos = bp;
    }
    else   { 
      term.sel_scroll = 0;
      if (p.x < 0 && p.y + term.disptop > term.sel_anchor.y)
        bp = (pos){.y = p.y - 1, .x = term.cols - 1};
    }
    sel_drag(get_selpoint(bp));
    win_update();
  }
  else if (term.mouse_state == MS_OPENING) {
    term.mouse_state = 0;
    term.selected = false;
    win_update();
  }
  else if (term.mouse_state > 0) {
    if (term.mouse_mode >= MM_BTN_EVENT)
      send_mouse_event(0x3F + term.mouse_state, mods, bp);
  }
  else {
    if (term.mouse_mode == MM_ANY_EVENT)
        send_mouse_event(0x43, mods, bp);
  }
}

void
term_mouse_wheel(int delta, int lines_per_notch, mod_keys mods, pos p)
{
  enum { NOTCH_DELTA = 120 };
  
  static int accu;
  accu += delta;
  
  if (!term.show_other_screen && is_app_mouse(&mods)) {
    // Send as mouse events, with one event per notch.
    int notches = accu / NOTCH_DELTA;
    if (notches) {
      accu -= NOTCH_DELTA * notches;
      char code = 0x60 | (notches < 0);
      notches = abs(notches);
      do send_mouse_event(code, mods, p); while (--notches);
    }
  }
  else if (mods == MDK_CTRL) {
    int zoom = accu / NOTCH_DELTA;
    if (zoom) {
      accu -= NOTCH_DELTA * zoom;
      win_zoom_font(zoom);
    }
  }
  else if (!(mods & ~MDK_SHIFT)) {
    // Scroll, taking the lines_per_notch setting into account.
    // Scroll by a page per notch if setting is -1 or Shift is pressed.
    if (lines_per_notch == -1 || mods & MDK_SHIFT)
      lines_per_notch = term.rows;
    int lines = lines_per_notch * accu / NOTCH_DELTA;
    if (lines) {
      accu -= lines * NOTCH_DELTA / lines_per_notch;
      if (!term.on_alt_screen || term.show_other_screen)
        term_scroll(0, -lines);
      else if (term.wheel_reporting) {
        // Send scroll distance as CSI a/b events
        bool up = lines > 0;
        lines = abs(lines);
        int pages = lines / term.rows;
        lines -= pages * term.rows;
        if (term.app_wheel) {
          send_keys(up ? "\e[1;2a" : "\e[1;2b", 6, pages);
          send_keys(up ? "\eOa" : "\eOb", 3, lines);
        }
        else {
          send_keys(up ? "\e[5~" : "\e[6~", 4, pages);
          char code[3] = 
            {'\e',  term.app_cursor_keys ? 'O' : '[', up ? 'A' : 'B'};
          send_keys(code, 3, lines);
        }
      }
    }
  }
}