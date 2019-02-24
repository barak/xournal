#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <math.h>
#include <string.h>
#include <gtk/gtk.h>
#include <libgnomecanvas/libgnomecanvas.h>

#include "xournal.h"
#include "xo-interface.h"
#include "xo-support.h"
#include "xo-callbacks.h"
#include "xo-misc.h"
#include "xo-file.h"
#include "xo-paint.h"

// some global constants

guint predef_colors_rgba[COLOR_MAX] =
  { 0x000000ff, 0x3333ccff, 0xff0000ff, 0x008000ff,
    0x808080ff, 0x00c0ffff, 0x00ff00ff, 0xff00ffff,
    0xff8000ff, 0xffff00ff, 0xffffffff };

guint predef_bgcolors_rgba[COLOR_MAX] = // meaningless ones set to white
  { 0xffffffff, 0xa0e8ffff, 0xffc0d4ff, 0x80ffc0ff,
    0xffffffff, 0xa0e8ffff, 0x80ffc0ff, 0xffc0d4ff,
    0xffc080ff, 0xffff80ff, 0xffffffff };

double predef_thickness[NUM_STROKE_TOOLS][THICKNESS_MAX] =
  { { 0.42, 0.85, 1.41,  2.26, 5.67 }, // pen thicknesses = 0.15, 0.3, 0.5, 0.8, 2 mm
    { 2.83, 2.83, 8.50, 19.84, 19.84 }, // eraser thicknesses = 1, 3, 7 mm
    { 2.83, 2.83, 8.50, 19.84, 19.84 }, // highlighter thicknesses = 1, 3, 7 mm
  };

// some manipulation functions

struct Page *new_page(struct Page *template)
{
  struct Page *pg = (struct Page *) g_memdup(template, sizeof(struct Page));
  struct Layer *l = g_new(struct Layer, 1);
  
  l->items = NULL;
  l->nitems = 0;
  pg->layers = g_list_append(NULL, l);
  pg->nlayers = 1;
  pg->bg = (struct Background *)g_memdup(template->bg, sizeof(struct Background));
  pg->bg->canvas_item = NULL;
  if (pg->bg->type == BG_PIXMAP || pg->bg->type == BG_PDF) {
    gdk_pixbuf_ref(pg->bg->pixbuf);
    refstring_ref(pg->bg->filename);
  }
  pg->group = (GnomeCanvasGroup *) gnome_canvas_item_new(
      gnome_canvas_root(canvas), gnome_canvas_clipgroup_get_type(), NULL);
  make_page_clipbox(pg);
  update_canvas_bg(pg);
  l->group = (GnomeCanvasGroup *) gnome_canvas_item_new(
      pg->group, gnome_canvas_group_get_type(), NULL);
  
  return pg;
}

/* Create a page from a background. 
   Note: bg should be an UNREFERENCED background.
   If needed, first duplicate it and increase the refcount of the pixbuf.
*/
struct Page *new_page_with_bg(struct Background *bg, double width, double height)
{
  struct Page *pg = g_new(struct Page, 1);
  struct Layer *l = g_new(struct Layer, 1);
  
  l->items = NULL;
  l->nitems = 0;
  pg->layers = g_list_append(NULL, l);
  pg->nlayers = 1;
  pg->bg = bg;
  pg->bg->canvas_item = NULL;
  pg->height = height;
  pg->width = width;
  pg->group = (GnomeCanvasGroup *) gnome_canvas_item_new(
      gnome_canvas_root(canvas), gnome_canvas_clipgroup_get_type(), NULL);
  make_page_clipbox(pg);
  update_canvas_bg(pg);
  l->group = (GnomeCanvasGroup *) gnome_canvas_item_new(
      pg->group, gnome_canvas_group_get_type(), NULL);
  
  return pg;
}

void realloc_cur_path(int n)
{
  if (n <= ui.cur_path_storage_alloc) return;
  ui.cur_path_storage_alloc = n+10;
  ui.cur_path.coords = g_realloc(ui.cur_path.coords, 2*(n+10)*sizeof(double));
}

// undo utility functions

void prepare_new_undo(void)
{
  struct UndoItem *u;
  // add a new UndoItem on the stack  
  u = (struct UndoItem *)g_malloc(sizeof(struct UndoItem));
  u->next = undo;
  u->multiop = 0;
  undo = u;
  ui.saved = FALSE;
  clear_redo_stack();
}

void clear_redo_stack(void)
{
  struct UndoItem *u;  
  GList *list, *repl;
  struct UndoErasureData *erasure;
  struct Item *it;

  /* Warning: the redo items might reference items from past redo entries,
     which have been destroyed before them. Be careful! As a rule, it's
     safe to destroy data which has been created at the current history step,
     it's unsafe to refer to any data from previous history steps */
  
  while (redo!=NULL) {
    if (redo->type == ITEM_STROKE) {
      gnome_canvas_points_free(redo->item->path);
      g_free(redo->item);
      /* the strokes are unmapped, so there are no associated canvas items */
    }
    else if (redo->type == ITEM_ERASURE) {
      for (list = redo->erasurelist; list!=NULL; list=list->next) {
        erasure = (struct UndoErasureData *)list->data;
        for (repl = erasure->replacement_items; repl!=NULL; repl=repl->next) {
          it = (struct Item *)repl->data;
          gnome_canvas_points_free(it->path);
          g_free(it);
        }
        g_list_free(erasure->replacement_items);
        g_free(erasure);
      }
      g_list_free(redo->erasurelist);
    }
    else if (redo->type == ITEM_NEW_BG_ONE || redo->type == ITEM_NEW_BG_RESIZE
          || redo->type == ITEM_NEW_DEFAULT_BG) {
      if (redo->bg->type == BG_PIXMAP || redo->bg->type == BG_PDF) {
        if (redo->bg->pixbuf!=NULL) gdk_pixbuf_unref(redo->bg->pixbuf);
        refstring_unref(redo->bg->filename);
      }
      g_free(redo->bg);
    }
    else if (redo->type == ITEM_NEW_PAGE) {
      redo->page->group = NULL;
      delete_page(redo->page);
    }
    else if (redo->type == ITEM_MOVESEL || redo->type == ITEM_REPAINTSEL) {
      g_list_free(redo->itemlist); g_list_free(redo->auxlist);
    }
    else if (redo->type == ITEM_PASTE) {
      for (list = redo->itemlist; list!=NULL; list=list->next) {
        it = (struct Item *)list->data;
        if (it->type == ITEM_STROKE) gnome_canvas_points_free(it->path);
        g_free(it);
      }
      g_list_free(redo->itemlist);
    }
    else if (redo->type == ITEM_NEW_LAYER) {
      g_free(redo->layer);
    }

    u = redo;
    redo = redo->next;
    g_free(u);
  }
  update_undo_redo_enabled();
}

void clear_undo_stack(void)
{
  struct UndoItem *u;
  GList *list;
  struct UndoErasureData *erasure;
  
  while (undo!=NULL) {
    // for strokes, items are already in the journal, so we don't free them
    // for erasures, we need to free the dead items
    if (undo->type == ITEM_ERASURE) {
      for (list = undo->erasurelist; list!=NULL; list=list->next) {
        erasure = (struct UndoErasureData *)list->data;
        gnome_canvas_points_free(erasure->item->path);
        g_free(erasure->item);
        g_list_free(erasure->replacement_items);
        g_free(erasure);
      }
      g_list_free(undo->erasurelist);
    }
    else if (undo->type == ITEM_NEW_BG_ONE || undo->type == ITEM_NEW_BG_RESIZE
          || undo->type == ITEM_NEW_DEFAULT_BG) {
      if (undo->bg->type == BG_PIXMAP || undo->bg->type == BG_PDF) {
        if (undo->bg->pixbuf!=NULL) gdk_pixbuf_unref(undo->bg->pixbuf);
        refstring_unref(undo->bg->filename);
      }
      g_free(undo->bg);
    }
    else if (undo->type == ITEM_MOVESEL || undo->type == ITEM_REPAINTSEL) {
      g_list_free(undo->itemlist); g_list_free(undo->auxlist);
    }
    else if (undo->type == ITEM_PASTE) {
      g_list_free(undo->itemlist);
    }
    else if (undo->type == ITEM_DELETE_LAYER) {
      undo->layer->group = NULL;
      delete_layer(undo->layer);
    }
    else if (undo->type == ITEM_DELETE_PAGE) {
      undo->page->group = NULL;
      delete_page(undo->page);
    }

    u = undo;
    undo = undo->next;
    g_free(u);
  }
  update_undo_redo_enabled();
}

// free data structures 

void delete_journal(struct Journal *j)
{
  while (j->pages!=NULL) {
    delete_page((struct Page *)j->pages->data);
    j->pages = g_list_delete_link(j->pages, j->pages);
  }
}

void delete_page(struct Page *pg)
{
  struct Layer *l;
  
  while (pg->layers!=NULL) {
    l = (struct Layer *)pg->layers->data;
    l->group = NULL;
    delete_layer(l);
    pg->layers = g_list_delete_link(pg->layers, pg->layers);
  }
  if (pg->group!=NULL) gtk_object_destroy(GTK_OBJECT(pg->group));
              // this also destroys the background's canvas items
  if (pg->bg->type == BG_PIXMAP || pg->bg->type == BG_PDF) {
    if (pg->bg->pixbuf != NULL) gdk_pixbuf_unref(pg->bg->pixbuf);
    if (pg->bg->filename != NULL) refstring_unref(pg->bg->filename);
  }
  g_free(pg->bg);
  g_free(pg);
}

void delete_layer(struct Layer *l)
{
  struct Item *item;
  
  while (l->items!=NULL) {
    item = (struct Item *)l->items->data;
    if (item->type == ITEM_STROKE && item->path != NULL) 
      gnome_canvas_points_free(item->path);
    // don't need to delete the canvas_item, as it's part of the group destroyed below
    g_free(item);
    l->items = g_list_delete_link(l->items, l->items);
  }
  if (l->group!= NULL) gtk_object_destroy(GTK_OBJECT(l->group));
  g_free(l);
}

// referenced strings

struct Refstring *new_refstring(const char *s)
{
  struct Refstring *rs = g_new(struct Refstring, 1);
  rs->nref = 1;
  if (s!=NULL) rs->s = g_strdup(s);
  else rs->s = NULL;
  rs->aux = NULL;
  return rs;
}

struct Refstring *refstring_ref(struct Refstring *rs)
{
  rs->nref++;
  return rs;
}

void refstring_unref(struct Refstring *rs)
{
  rs->nref--;
  if (rs->nref == 0) {
    if (rs->s!=NULL) g_free(rs->s);
    if (rs->aux!=NULL) g_free(rs->aux);
    g_free(rs);
  }
}


// some helper functions

void get_pointer_coords(GdkEvent *event, gdouble *ret)
{
  double x, y;
  gdk_event_get_coords(event, &x, &y);
  gnome_canvas_window_to_world(canvas, x, y, ret, ret+1);
  ret[0] -= ui.cur_page->hoffset;
  ret[1] -= ui.cur_page->voffset;
}

void fix_xinput_coords(GdkEvent *event)
{
  double *axes, *px, *py, axis_width;
  GdkDevice *device;
  int wx, wy, sx, sy;
  
  if (event->type == GDK_BUTTON_PRESS || event->type == GDK_BUTTON_RELEASE) {
    axes = event->button.axes;
    px = &(event->button.x);
    py = &(event->button.y);
    device = event->button.device;
  }
  else if (event->type == GDK_MOTION_NOTIFY) {
    axes = event->motion.axes;
    px = &(event->motion.x);
    py = &(event->motion.y);
    device = event->motion.device;
  }
  else return; // nothing we know how to do
  
  gdk_window_get_origin(event->any.window, &wx, &wy);
  gnome_canvas_get_scroll_offsets(canvas, &sx, &sy);
  
  axis_width = device->axes[0].max - device->axes[0].min;
  if (axis_width>EPSILON)
    *px = (axes[0]/axis_width)*ui.screen_width + sx - wx;

  axis_width = device->axes[1].max - device->axes[1].min;
  if (axis_width>EPSILON)
    *py = (axes[1]/axis_width)*ui.screen_height + sy - wy;
}

void update_item_bbox(struct Item *item)
{
  int i;
  double *p;
  
  if (item->type == ITEM_STROKE) {
    item->bbox.left = item->bbox.right = item->path->coords[0];
    item->bbox.top = item->bbox.bottom = item->path->coords[1];
    for (i=1, p=item->path->coords+2; i<item->path->num_points; i++, p+=2)
    {
      if (p[0] < item->bbox.left) item->bbox.left = p[0];
      if (p[0] > item->bbox.right) item->bbox.right = p[0];
      if (p[1] < item->bbox.top) item->bbox.top = p[1];
      if (p[1] > item->bbox.bottom) item->bbox.bottom = p[1];
    }
  }
}

void make_page_clipbox(struct Page *pg)
{
  GnomeCanvasPathDef *pg_clip;
  
  pg_clip = gnome_canvas_path_def_new_sized(4);
  gnome_canvas_path_def_moveto(pg_clip, 0., 0.);
  gnome_canvas_path_def_lineto(pg_clip, 0., pg->height);
  gnome_canvas_path_def_lineto(pg_clip, pg->width, pg->height);
  gnome_canvas_path_def_lineto(pg_clip, pg->width, 0.);
  gnome_canvas_path_def_closepath(pg_clip);
  gnome_canvas_item_set(GNOME_CANVAS_ITEM(pg->group), "path", pg_clip, NULL);
  gnome_canvas_path_def_unref(pg_clip);
}

void make_canvas_items(void)
{
  struct Page *pg;
  struct Layer *l;
  struct Item *item;
  GList *pagelist, *layerlist, *itemlist;
  
  for (pagelist = journal.pages; pagelist!=NULL; pagelist = pagelist->next) {
    pg = (struct Page *)pagelist->data;
    if (pg->group == NULL) {
      pg->group = (GnomeCanvasGroup *) gnome_canvas_item_new(
         gnome_canvas_root(canvas), gnome_canvas_clipgroup_get_type(), NULL);
      make_page_clipbox(pg);
    }
    if (pg->bg->canvas_item == NULL) update_canvas_bg(pg);
    for (layerlist = pg->layers; layerlist!=NULL; layerlist = layerlist->next) {
      l = (struct Layer *)layerlist->data;
      if (l->group == NULL)
        l->group = (GnomeCanvasGroup *) gnome_canvas_item_new(
           pg->group, gnome_canvas_group_get_type(), NULL);
      for (itemlist = l->items; itemlist!=NULL; itemlist = itemlist->next) {
        item = (struct Item *)itemlist->data;
        if (item->type == ITEM_STROKE && item->canvas_item == NULL) {
          item->canvas_item = gnome_canvas_item_new(l->group,
              gnome_canvas_line_get_type(), "points", item->path,
              "cap-style", GDK_CAP_ROUND, "join-style", GDK_JOIN_ROUND,
              "fill-color-rgba", item->brush.color_rgba,
              "width-units", item->brush.thickness, NULL);
        }
      }
    }
  }
}

void update_canvas_bg(struct Page *pg)
{
  GnomeCanvasGroup *group;
  GnomeCanvasPoints *seg;
  GdkPixbuf *scaled_pix;
  double *pt;
  double x, y;
  int w, h;
  
  if (pg->bg->canvas_item != NULL)
    gtk_object_destroy(GTK_OBJECT(pg->bg->canvas_item));
  pg->bg->canvas_item = NULL;
  
  if (pg->bg->type == BG_SOLID)
  {
    pg->bg->canvas_item = gnome_canvas_item_new(pg->group,
                               gnome_canvas_group_get_type(), NULL);
    group = GNOME_CANVAS_GROUP(pg->bg->canvas_item);
    lower_canvas_item_to(pg->group, pg->bg->canvas_item, NULL);
    gnome_canvas_item_new(group, gnome_canvas_rect_get_type(),
      "x1", 0., "x2", pg->width, "y1", 0., "y2", pg->height,
      "fill-color-rgba", pg->bg->color_rgba, NULL);
    if (pg->bg->ruling == RULING_NONE) return;
    seg = gnome_canvas_points_new(2);
    pt = seg->coords;
    if (pg->bg->ruling == RULING_GRAPH) {
      pt[1] = 0; pt[3] = pg->height;
      for (x=RULING_GRAPHSPACING; x<pg->width-1; x+=RULING_GRAPHSPACING) {
        pt[0] = pt[2] = x;
        gnome_canvas_item_new(group, gnome_canvas_line_get_type(),
           "points", seg, "fill-color-rgba", RULING_COLOR,
           "width-units", RULING_THICKNESS, NULL);
      }      
      pt[0] = 0; pt[2] = pg->width;
      for (y=RULING_GRAPHSPACING; y<pg->height-1; y+=RULING_GRAPHSPACING) {
        pt[1] = pt[3] = y;
        gnome_canvas_item_new(group, gnome_canvas_line_get_type(),
           "points", seg, "fill-color-rgba", RULING_COLOR,
           "width-units", RULING_THICKNESS, NULL);
      }      
      gnome_canvas_points_free(seg);
      return;
    }
    pt[0] = 0; pt[2] = pg->width;
    for (y=RULING_TOPMARGIN; y<pg->height-1; y+=RULING_SPACING) {
      pt[1] = pt[3] = y;
      gnome_canvas_item_new(group, gnome_canvas_line_get_type(),
         "points", seg, "fill-color-rgba", RULING_COLOR,
         "width-units", RULING_THICKNESS, NULL);
    }      
    if (pg->bg->ruling == RULING_LINED) {
      pt[0] = pt[2] = RULING_LEFTMARGIN;
      pt[1] = 0; pt[3] = pg->height;
      gnome_canvas_item_new(group, gnome_canvas_line_get_type(),
         "points", seg, "fill-color-rgba", RULING_MARGIN_COLOR,
         "width-units", RULING_THICKNESS, NULL);
    }
    gnome_canvas_points_free(seg);
    return;
  }
  
  if (pg->bg->type == BG_PIXMAP)
  {
    if (ui.antialias_bg) {
      set_cursor_busy(TRUE);
      w = (int)floor(pg->width*ui.zoom+0.5);
      h = (int)floor(pg->height*ui.zoom+0.5);
      if (w == gdk_pixbuf_get_width(pg->bg->pixbuf) &&
          h == gdk_pixbuf_get_height(pg->bg->pixbuf))
        scaled_pix = gdk_pixbuf_ref(pg->bg->pixbuf);
      else
        scaled_pix = gdk_pixbuf_scale_simple(pg->bg->pixbuf, w, h, GDK_INTERP_BILINEAR);
      pg->bg->pixbuf_scale = ui.zoom;
      set_cursor_busy(FALSE);
    }
    else {
      scaled_pix = gdk_pixbuf_ref(pg->bg->pixbuf);
      pg->bg->pixbuf_scale = 0;
    }
    pg->bg->canvas_item = gnome_canvas_item_new(pg->group, 
        gnome_canvas_pixbuf_get_type(), 
        "pixbuf", scaled_pix,
        "width", pg->width, "height", pg->height, 
        "width-set", TRUE, "height-set", TRUE, 
        NULL);
    gdk_pixbuf_unref(scaled_pix);
    lower_canvas_item_to(pg->group, pg->bg->canvas_item, NULL);
  }

  if (pg->bg->type == BG_PDF)
  {
    if (pg->bg->pixbuf == NULL) return;
    pg->bg->canvas_item = gnome_canvas_item_new(pg->group, 
        gnome_canvas_pixbuf_get_type(), 
        "pixbuf", pg->bg->pixbuf,
        "width", pg->width, "height", pg->height, 
        "width-set", TRUE, "height-set", TRUE, 
        NULL);
    lower_canvas_item_to(pg->group, pg->bg->canvas_item, NULL);
  }

}

gboolean is_visible(struct Page *pg)
{
  GtkAdjustment *v_adj;
  double ytop, ybot;
  
  if (!ui.view_continuous) return (pg == ui.cur_page);
  v_adj = gtk_layout_get_vadjustment(GTK_LAYOUT(canvas));
  ytop = v_adj->value/ui.zoom;
  ybot = (v_adj->value + v_adj->page_size) / ui.zoom;
  return (MAX(ytop, pg->voffset) < MIN(ybot, pg->voffset+pg->height));
}

void rescale_bg_pixmaps(void)
{
  GList *pglist;
  struct Page *pg;
  GdkPixbuf *pix;
  
  for (pglist = journal.pages; pglist!=NULL; pglist = pglist->next) {
    pg = (struct Page *)pglist->data;
    // in progressive mode we scale only visible pages
    if (ui.progressive_bg && !is_visible(pg)) continue;

    if (pg->bg->type == BG_PIXMAP) { // do the rescaling ourselves
      if (ui.antialias_bg) {
        if (pg->bg->pixbuf_scale == ui.zoom) continue;
        set_cursor_busy(TRUE);
        pix = gdk_pixbuf_scale_simple(pg->bg->pixbuf,
          (int)floor(pg->width*ui.zoom+0.5), (int)floor(pg->height*ui.zoom+0.5),
          GDK_INTERP_BILINEAR);
        gnome_canvas_item_set(pg->bg->canvas_item, "pixbuf", pix, NULL);
        gdk_pixbuf_unref(pix);
        pg->bg->pixbuf_scale = ui.zoom;
        set_cursor_busy(FALSE);
      } else
      {
        g_object_get(G_OBJECT(pg->bg->canvas_item), "pixbuf", &pix, NULL);
        if (pix!=pg->bg->pixbuf)
          gnome_canvas_item_set(pg->bg->canvas_item, "pixbuf", pg->bg->pixbuf, NULL);
        pg->bg->pixbuf_scale = 0;
      }
    }
    if (pg->bg->type == BG_PDF) { // request an asynchronous update
      if (pg->bg->pixbuf_scale == ui.zoom) continue;
      add_bgpdf_request(pg->bg->file_page_seq, ui.zoom, FALSE);
      pg->bg->pixbuf_scale = ui.zoom;
    }
  }
}

gboolean have_intersect(struct BBox *a, struct BBox *b)
{
  return (MAX(a->top, b->top) <= MIN(a->bottom, b->bottom)) &&
         (MAX(a->left, b->left) <= MIN(a->right, b->right));
}

/* In libgnomecanvas 2.10.0, the lower/raise functions fail to update
   correctly the end of the group's item list. We try to work around this.
   DON'T USE gnome_canvas_item_raise/lower directly !! */

void lower_canvas_item_to(GnomeCanvasGroup *g, GnomeCanvasItem *item, GnomeCanvasItem *after)
{
  int i1, i2;
  
  i1 = g_list_index(g->item_list, item);
  if (i1 == -1) return;
  
  if (after == NULL) i2 = -1;
  else i2 = g_list_index(g->item_list, after);

  if (i1 < i2) gnome_canvas_item_raise(item, i2-i1);
  if (i1 > i2+1) gnome_canvas_item_lower(item, i1-i2-1);
  
  // BUGFIX for libgnomecanvas
  g->item_list_end = g_list_last(g->item_list);
}

// some interface functions

void update_thickness_buttons(void)
{
  if (ui.selection!=NULL || ui.toolno[ui.cur_mapping] >= NUM_STROKE_TOOLS) {
    gtk_toggle_tool_button_set_active(
      GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonThicknessOther")), TRUE);
  } else 
  switch (ui.cur_brush->thickness_no) {
    case THICKNESS_FINE:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonFine")), TRUE);
      break;
    case THICKNESS_MEDIUM:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonMedium")), TRUE);
      break;
    case THICKNESS_THICK:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonThick")), TRUE);
      break;
    default:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonThicknessOther")), TRUE);
  }
}

void update_color_buttons(void)
{
  if (ui.selection!=NULL || (ui.toolno[ui.cur_mapping] != TOOL_PEN 
                          && ui.toolno[ui.cur_mapping] != TOOL_HIGHLIGHTER)) {
    gtk_toggle_tool_button_set_active(
      GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonColorOther")), TRUE);
  } else
  switch (ui.cur_brush->color_no) {
    case COLOR_BLACK:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonBlack")), TRUE);
      break;
    case COLOR_BLUE:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonBlue")), TRUE);
      break;
    case COLOR_RED:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonRed")), TRUE);
      break;
    case COLOR_GREEN:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonGreen")), TRUE);
      break;
    case COLOR_GRAY:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonGray")), TRUE);
      break;
    case COLOR_LIGHTBLUE:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonLightBlue")), TRUE);
      break;
    case COLOR_LIGHTGREEN:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonLightGreen")), TRUE);
      break;
    case COLOR_MAGENTA:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonMagenta")), TRUE);
      break;
    case COLOR_ORANGE:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonOrange")), TRUE);
      break;
    case COLOR_YELLOW:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonYellow")), TRUE);
      break;
    case COLOR_WHITE:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonWhite")), TRUE);
      break;
    default:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonColorOther")), TRUE);
  }
}

void update_tool_buttons(void)
{
  switch(ui.toolno[ui.cur_mapping]) {
    case TOOL_PEN:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonPen")), TRUE);
      break;
    case TOOL_ERASER:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonEraser")), TRUE);
      break;
    case TOOL_HIGHLIGHTER:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonHighlighter")), TRUE);
      break;
    case TOOL_TEXT:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonText")), TRUE);
      break;
    case TOOL_SELECTREGION:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonSelectRegion")), TRUE);
      break;
    case TOOL_SELECTRECT:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonSelectRectangle")), TRUE);
      break;
    case TOOL_VERTSPACE:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonVerticalSpace")), TRUE);
      break;
    case TOOL_HAND:
      gtk_toggle_tool_button_set_active(
        GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonHand")), TRUE);
      break;
  }
    
  gtk_toggle_tool_button_set_active(
    GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonRuler")), ui.ruler[ui.cur_mapping]);
  update_thickness_buttons();
  update_color_buttons();
}

void update_tool_menu(void)
{
  switch(ui.toolno[0]) {
    case TOOL_PEN:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsPen")), TRUE);
      break;
    case TOOL_ERASER:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsEraser")), TRUE);
      break;
    case TOOL_HIGHLIGHTER:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsHighlighter")), TRUE);
      break;
    case TOOL_TEXT:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsText")), TRUE);
      break;
    case TOOL_SELECTREGION:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsSelectRegion")), TRUE);
      break;
    case TOOL_SELECTRECT:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsSelectRectangle")), TRUE);
      break;
    case TOOL_VERTSPACE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsVerticalSpace")), TRUE);
      break;
    case TOOL_HAND:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsHand")), TRUE);
      break;
  }

  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsRuler")), ui.ruler[0]);
}

void update_ruler_indicator(void)
{
  gtk_toggle_tool_button_set_active(
    GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonRuler")), ui.ruler[ui.cur_mapping]);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("toolsRuler")), ui.ruler[0]);
}

void update_color_menu(void)
{
  if (ui.selection!=NULL || (ui.toolno[ui.cur_mapping] != TOOL_PEN 
                          && ui.toolno[ui.cur_mapping] != TOOL_HIGHLIGHTER)) {
    gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorNA")), TRUE);
  } else
  switch (ui.cur_brush->color_no) {
    case COLOR_BLACK:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorBlack")), TRUE);
      break;
    case COLOR_BLUE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorBlue")), TRUE);
      break;
    case COLOR_RED:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorRed")), TRUE);
      break;
    case COLOR_GREEN:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorGreen")), TRUE);
      break;
    case COLOR_GRAY:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorGray")), TRUE);
      break;
    case COLOR_LIGHTBLUE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorLightBlue")), TRUE);
      break;
    case COLOR_LIGHTGREEN:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorLightGreen")), TRUE);
      break;
    case COLOR_MAGENTA:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorMagenta")), TRUE);
      break;
    case COLOR_ORANGE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorOrange")), TRUE);
      break;
    case COLOR_YELLOW:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorYellow")), TRUE);
      break;
    case COLOR_WHITE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorWhite")), TRUE);
      break;
    default:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("colorOther")), TRUE);
  }
}

void update_pen_props_menu(void)
{
  switch(ui.brushes[0][TOOL_PEN].thickness_no) {
    case THICKNESS_VERYFINE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("penthicknessVeryFine")), TRUE);
      break;
    case THICKNESS_FINE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("penthicknessFine")), TRUE);
      break;
    case THICKNESS_MEDIUM:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("penthicknessMedium")), TRUE);
      break;
    case THICKNESS_THICK:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("penthicknessThick")), TRUE);
      break;
    case THICKNESS_VERYTHICK:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("penthicknessVeryThick")), TRUE);
      break;
  }
}

void update_eraser_props_menu(void)
{
  switch (ui.brushes[0][TOOL_ERASER].thickness_no) {
    case THICKNESS_FINE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("eraserFine")), TRUE);
      break;
    case THICKNESS_MEDIUM:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("eraserMedium")), TRUE);
      break;
    case THICKNESS_THICK:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("eraserThick")), TRUE);
      break;
  }
  
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("eraserStandard")),
    ui.brushes[0][TOOL_ERASER].tool_options == TOOLOPT_ERASER_STANDARD);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("eraserWhiteout")),
    ui.brushes[0][TOOL_ERASER].tool_options == TOOLOPT_ERASER_WHITEOUT);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("eraserDeleteStrokes")),
    ui.brushes[0][TOOL_ERASER].tool_options == TOOLOPT_ERASER_STROKES);
}

void update_highlighter_props_menu(void)
{
  switch (ui.brushes[0][TOOL_HIGHLIGHTER].thickness_no) {
    case THICKNESS_FINE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("highlighterFine")), TRUE);
      break;
    case THICKNESS_MEDIUM:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("highlighterMedium")), TRUE);
      break;
    case THICKNESS_THICK:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("highlighterThick")), TRUE);
      break;
  }
}

void update_mappings_menu_linkings(void)
{
  switch (ui.linked_brush[1]) {
    case BRUSH_LINKED:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2LinkBrush")), TRUE);
      break;
    case BRUSH_COPIED:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2CopyBrush")), TRUE);
      break;
    case BRUSH_STATIC:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2NABrush")), TRUE);
      break;
  }
  switch (ui.linked_brush[2]) {
    case BRUSH_LINKED:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3LinkBrush")), TRUE);
      break;
    case BRUSH_COPIED:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3CopyBrush")), TRUE);
      break;
    case BRUSH_STATIC:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3NABrush")), TRUE);
      break;
  }
}

void update_mappings_menu(void)
{
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("optionsButtonMappings")), ui.use_erasertip);

  switch(ui.toolno[1]) {
    case TOOL_PEN:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2Pen")), TRUE);
      break;
    case TOOL_ERASER:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2Eraser")), TRUE);
      break;
    case TOOL_HIGHLIGHTER:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2Highlighter")), TRUE);
      break;
    case TOOL_TEXT:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2Text")), TRUE);
      break;
    case TOOL_SELECTREGION:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2SelectRegion")), TRUE);
      break;
    case TOOL_SELECTRECT:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2SelectRectangle")), TRUE);
      break;
    case TOOL_VERTSPACE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button2VerticalSpace")), TRUE);
      break;
  }
  switch(ui.toolno[2]) {
    case TOOL_PEN:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3Pen")), TRUE);
      break;
    case TOOL_ERASER:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3Eraser")), TRUE);
      break;
    case TOOL_HIGHLIGHTER:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3Highlighter")), TRUE);
      break;
    case TOOL_TEXT:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3Text")), TRUE);
      break;
    case TOOL_SELECTREGION:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3SelectRegion")), TRUE);
      break;
    case TOOL_SELECTRECT:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3SelectRectangle")), TRUE);
      break;
    case TOOL_VERTSPACE:
      gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(GET_COMPONENT("button3VerticalSpace")), TRUE);
      break;
  }
  update_mappings_menu_linkings();
}

void do_switch_page(int pg, gboolean rescroll, gboolean refresh_all)
{
  int i;
  struct Layer *layer;
  GList *list;
  
  ui.pageno = pg;

  /* re-show all the layers of the old page */
  if (ui.cur_page != NULL)
    for (i=0, list = ui.cur_page->layers; list!=NULL; i++, list = list->next) {
      layer = (struct Layer *)list->data;
      if (layer->group!=NULL)
        gnome_canvas_item_show(GNOME_CANVAS_ITEM(layer->group));
    }
  
  ui.cur_page = g_list_nth_data(journal.pages, ui.pageno);
  ui.layerno = ui.cur_page->nlayers-1;
  ui.cur_layer = (struct Layer *)(g_list_last(ui.cur_page->layers)->data);
  update_page_stuff();
  if (ui.progressive_bg) rescale_bg_pixmaps();
 
  if (rescroll) { // scroll and force a refresh
    gtk_adjustment_set_value(gtk_layout_get_vadjustment(GTK_LAYOUT(canvas)),
      ui.cur_page->voffset*ui.zoom);
    if (refresh_all) 
      gnome_canvas_set_pixels_per_unit(canvas, ui.zoom);
    else if (!ui.view_continuous)
      gnome_canvas_item_move(GNOME_CANVAS_ITEM(ui.cur_page->group), 0., 0.);
  }
}

void update_page_stuff(void)
{
  gchar tmp[10];
  GtkComboBox *layerbox;
  int i;
  GList *pglist;
  GtkSpinButton *spin;
  struct Page *pg;
  double vertpos, maxwidth;
  struct Layer *layer;
  int relscroll;

  // move the page groups to their rightful locations or hide them
  if (ui.view_continuous) {
    vertpos = 0.; 
    maxwidth = 0.;
    for (i=0, pglist = journal.pages; pglist!=NULL; i++, pglist = pglist->next) {
      pg = (struct Page *)pglist->data;
      if (pg->group!=NULL) {
        pg->hoffset = 0.; pg->voffset = vertpos;
        gnome_canvas_item_set(GNOME_CANVAS_ITEM(pg->group), 
            "x", pg->hoffset, "y", pg->voffset, NULL);
        gnome_canvas_item_show(GNOME_CANVAS_ITEM(pg->group));
      }
      vertpos += pg->height + VIEW_CONTINUOUS_SKIP;
      if (pg->width > maxwidth) maxwidth = pg->width;
    }
    vertpos -= VIEW_CONTINUOUS_SKIP;
    gnome_canvas_set_scroll_region(canvas, 0, 0, maxwidth, vertpos);
  } else {
    for (pglist = journal.pages; pglist!=NULL; pglist = pglist->next) {
      pg = (struct Page *)pglist->data;
      if (pg == ui.cur_page && pg->group!=NULL) {
        pg->hoffset = 0.; pg->voffset = 0.;
        gnome_canvas_item_set(GNOME_CANVAS_ITEM(pg->group), 
            "x", pg->hoffset, "y", pg->voffset, NULL);
        gnome_canvas_item_show(GNOME_CANVAS_ITEM(pg->group));
      } else {
        if (pg->group!=NULL) gnome_canvas_item_hide(GNOME_CANVAS_ITEM(pg->group));
      }
    }
    gnome_canvas_set_scroll_region(canvas, 0, 0, ui.cur_page->width, ui.cur_page->height);
  }

  // update the page / layer info at bottom of screen

  spin = GTK_SPIN_BUTTON(GET_COMPONENT("spinPageNo"));
  ui.in_update_page_stuff = TRUE; // avoid a bad retroaction
  gtk_spin_button_set_range(spin, 1, journal.npages+1);
    /* npages+1 will be used to create a new page at end */
  gtk_spin_button_set_value(spin, ui.pageno+1);
  g_snprintf(tmp, 10, " of %d", journal.npages);
  gtk_label_set_text(GTK_LABEL(GET_COMPONENT("labelNumpages")), tmp);

  layerbox = GTK_COMBO_BOX(GET_COMPONENT("comboLayer"));
  if (ui.layerbox_length == 0) {
    gtk_combo_box_prepend_text(layerbox, "Background");
    ui.layerbox_length++;
  }
  while (ui.layerbox_length > ui.cur_page->nlayers+1) {
    gtk_combo_box_remove_text(layerbox, 0);
    ui.layerbox_length--;
  }
  while (ui.layerbox_length < ui.cur_page->nlayers+1) {
    g_snprintf(tmp, 10, "Layer %d", ui.layerbox_length++);
    gtk_combo_box_prepend_text(layerbox, tmp);
  }
  gtk_combo_box_set_active(layerbox, ui.cur_page->nlayers-1-ui.layerno);
  ui.in_update_page_stuff = FALSE;
  
  // update the paper-style menu radio buttons
  
  if (ui.view_continuous)
    gtk_check_menu_item_set_active(
       GTK_CHECK_MENU_ITEM(GET_COMPONENT("viewContinuous")), TRUE);
  else
    gtk_check_menu_item_set_active(
       GTK_CHECK_MENU_ITEM(GET_COMPONENT("viewOnePage")), TRUE);

  if (ui.cur_page->bg->type == BG_SOLID && !ui.bg_apply_all_pages) {
    switch (ui.cur_page->bg->color_no) {
      case COLOR_WHITE:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorWhite")), TRUE);
        break;
      case COLOR_YELLOW:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorYellow")), TRUE);
        break;
      case COLOR_RED:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorPink")), TRUE);
        break;
      case COLOR_ORANGE:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorOrange")), TRUE);
        break;
      case COLOR_BLUE:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorBlue")), TRUE);
        break;
      case COLOR_GREEN:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorGreen")), TRUE);
        break;
      default:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorOther")), TRUE);
        break;
    }
    switch (ui.cur_page->bg->ruling) {
      case RULING_NONE:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("paperstylePlain")), TRUE);
        break;
      case RULING_LINED:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("paperstyleLined")), TRUE);
        break;
      case RULING_RULED:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("paperstyleRuled")), TRUE);
        break;
      case RULING_GRAPH:
        gtk_check_menu_item_set_active(
          GTK_CHECK_MENU_ITEM(GET_COMPONENT("paperstyleGraph")), TRUE);
        break;
    }
  } else {
    gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorNA")), TRUE);
    gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(GET_COMPONENT("paperstyleNA")), TRUE);
  }
  
  // enable/disable the page/layer menu items and toolbar buttons

  gtk_widget_set_sensitive(GET_COMPONENT("journalPaperColor"), 
     ui.cur_page->bg->type == BG_SOLID || ui.bg_apply_all_pages);
  gtk_widget_set_sensitive(GET_COMPONENT("journalSetAsDefault"),
     ui.cur_page->bg->type == BG_SOLID);
  
  gtk_widget_set_sensitive(GET_COMPONENT("viewFirstPage"), ui.pageno!=0);
  gtk_widget_set_sensitive(GET_COMPONENT("viewPreviousPage"), ui.pageno!=0);
  gtk_widget_set_sensitive(GET_COMPONENT("viewNextPage"), TRUE);
  gtk_widget_set_sensitive(GET_COMPONENT("viewLastPage"), ui.pageno!=journal.npages-1);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonFirstPage"), ui.pageno!=0);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonPreviousPage"), ui.pageno!=0);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonNextPage"), TRUE);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonLastPage"), ui.pageno!=journal.npages-1);
  
  gtk_widget_set_sensitive(GET_COMPONENT("viewShowLayer"), ui.layerno!=ui.cur_page->nlayers-1);
  gtk_widget_set_sensitive(GET_COMPONENT("viewHideLayer"), ui.layerno>=0);

  gtk_widget_set_sensitive(GET_COMPONENT("editPaste"), ui.cur_layer!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonPaste"), ui.cur_layer!=NULL);
}

void update_toolbar_and_menu(void)
{
  update_tool_buttons(); // takes care of other toolbar buttons as well  
  update_tool_menu();
  update_color_menu();
  update_pen_props_menu();
  update_eraser_props_menu();
  update_highlighter_props_menu();
  update_mappings_menu();

  gtk_toggle_tool_button_set_active(
    GTK_TOGGLE_TOOL_BUTTON(GET_COMPONENT("buttonFullscreen")), ui.fullscreen);
  gtk_check_menu_item_set_active(
    GTK_CHECK_MENU_ITEM(GET_COMPONENT("viewFullscreen")), ui.fullscreen);
}

void update_file_name(char *filename)
{
  gchar tmp[100], *p;
  if (ui.filename != NULL) g_free(ui.filename);
  ui.filename = filename;
  if (filename == NULL) {
    gtk_window_set_title(GTK_WINDOW (winMain), "Xournal");
    return;
  }
  p = g_utf8_strrchr(filename, -1, '/');
  if (p == NULL) p = filename; 
  else p = g_utf8_next_char(p);
  g_snprintf(tmp, 100, "Xournal - %s", p);
  gtk_window_set_title(GTK_WINDOW (winMain), tmp);
  new_mru_entry(filename);
}

void update_undo_redo_enabled(void)
{
  gtk_widget_set_sensitive(GET_COMPONENT("editUndo"), undo!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("editRedo"), redo!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonUndo"), undo!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonRedo"), redo!=NULL);
}

void update_copy_paste_enabled(void)
{
  gtk_widget_set_sensitive(GET_COMPONENT("editCut"), ui.selection!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("editCopy"), ui.selection!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("editDelete"), ui.selection!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonCut"), ui.selection!=NULL);
  gtk_widget_set_sensitive(GET_COMPONENT("buttonCopy"), ui.selection!=NULL);
}

void update_mapping_linkings(int toolno)
{
  int i;
  
  for (i = 1; i<=NUM_BUTTONS; i++) {
    if (ui.linked_brush[i] == BRUSH_LINKED) {
      if (toolno >= 0 && toolno < NUM_STROKE_TOOLS)
        g_memmove(&(ui.brushes[i][toolno]), &(ui.brushes[0][toolno]), sizeof(struct Brush));
      ui.ruler[i] = ui.ruler[0];
      if (ui.toolno[i]!=TOOL_PEN && ui.toolno[i]!=TOOL_HIGHLIGHTER)
        ui.ruler[i] = FALSE;
    }
    if (ui.linked_brush[i] == BRUSH_COPIED && toolno == ui.toolno[i]) {
      ui.linked_brush[i] = BRUSH_STATIC;
      if (i==1 || i==2) update_mappings_menu_linkings();
    }
  }
}

void set_cur_color(int color)
{
  ui.cur_brush->color_no = color;
  if (ui.toolno[0] == TOOL_HIGHLIGHTER)
    ui.cur_brush->color_rgba = predef_colors_rgba[color] & HILITER_ALPHA_MASK;
  else
    ui.cur_brush->color_rgba = predef_colors_rgba[color];
  update_mapping_linkings(ui.toolno[0]);
}

void process_color_activate(GtkMenuItem *menuitem, int color)
{
  if (GTK_OBJECT_TYPE(menuitem) == GTK_TYPE_RADIO_MENU_ITEM) {
    if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM (menuitem)))
      return;
  } else {
    if (!gtk_toggle_tool_button_get_active(GTK_TOGGLE_TOOL_BUTTON (menuitem)))
      return;
  }

  if (ui.cur_mapping != 0) return; // not user-generated

  if (ui.selection != NULL) {
    recolor_selection(color);
    update_color_buttons();
    update_color_menu();
  }
  
  if (ui.toolno[0] != TOOL_PEN && ui.toolno[0] != TOOL_HIGHLIGHTER) {
    if (ui.selection != NULL) return;
    ui.toolno[0] = TOOL_PEN;
    ui.cur_brush = &(ui.brushes[0][TOOL_PEN]);
    update_tool_buttons();
    update_tool_menu();
  }
  
  set_cur_color(color);
  update_color_buttons();
  update_color_menu();
  update_cursor();
}

void process_thickness_activate(GtkMenuItem *menuitem, int tool, int val)
{
  if (GTK_OBJECT_TYPE(menuitem) == GTK_TYPE_RADIO_MENU_ITEM) {
    if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM (menuitem)))
      return;
  } else {
    if (!gtk_toggle_tool_button_get_active(GTK_TOGGLE_TOOL_BUTTON (menuitem)))
      return;
  }

  if (ui.cur_mapping != 0) return; // not user-generated

  if (ui.selection != NULL && GTK_OBJECT_TYPE(menuitem) != GTK_TYPE_RADIO_MENU_ITEM) {
    rethicken_selection(val);
    update_thickness_buttons();
  }

  if (tool >= NUM_STROKE_TOOLS) {
    update_thickness_buttons(); // undo illegal button selection
    return;
  }

  ui.brushes[0][tool].thickness_no = val;
  ui.brushes[0][tool].thickness = predef_thickness[tool][val];
  update_mapping_linkings(tool);
  
  update_thickness_buttons();
  if (tool == TOOL_PEN) update_pen_props_menu();
  if (tool == TOOL_ERASER) update_eraser_props_menu();
  if (tool == TOOL_HIGHLIGHTER) update_highlighter_props_menu();
  update_cursor();
}

void process_papercolor_activate(GtkMenuItem *menuitem, int color)
{
  struct Page *pg;
  GList *pglist;
  gboolean hasdone;

  if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM (menuitem)))
    return;

  if ((ui.cur_page->bg->type != BG_SOLID) || ui.bg_apply_all_pages)
    gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(GET_COMPONENT("papercolorNA")), TRUE);

  pg = ui.cur_page;
  hasdone = FALSE;
  for (pglist = journal.pages; pglist!=NULL; pglist = pglist->next) {
    if (ui.bg_apply_all_pages) pg = (struct Page *)pglist->data;
    if (pg->bg->type == BG_SOLID && pg->bg->color_no != color) {
      prepare_new_undo();
      if (hasdone) undo->multiop |= MULTIOP_CONT_UNDO;
      undo->multiop |= MULTIOP_CONT_REDO;
      hasdone = TRUE;
      undo->type = ITEM_NEW_BG_ONE;
      undo->page = pg;
      undo->bg = (struct Background *)g_memdup(pg->bg, sizeof(struct Background));
      undo->bg->canvas_item = NULL;

      pg->bg->color_no = color;
      pg->bg->color_rgba = predef_bgcolors_rgba[color];
      update_canvas_bg(pg);
    }
    if (!ui.bg_apply_all_pages) break;
  }
  if (hasdone) undo->multiop -= MULTIOP_CONT_REDO;
}

void process_paperstyle_activate(GtkMenuItem *menuitem, int style)
{
  struct Page *pg;
  GList *pglist;
  gboolean hasdone, must_upd;

  if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM (menuitem)))
    return;

  if (ui.bg_apply_all_pages)
    gtk_check_menu_item_set_active(
      GTK_CHECK_MENU_ITEM(GET_COMPONENT("paperstyleNA")), TRUE);

  pg = ui.cur_page;
  hasdone = FALSE;
  must_upd = FALSE;
  for (pglist = journal.pages; pglist!=NULL; pglist = pglist->next) {
    if (ui.bg_apply_all_pages) pg = (struct Page *)pglist->data;
    if (pg->bg->type != BG_SOLID || pg->bg->ruling != style) {
      prepare_new_undo();
      undo->type = ITEM_NEW_BG_ONE;
      if (hasdone) undo->multiop |= MULTIOP_CONT_UNDO;
      undo->multiop |= MULTIOP_CONT_REDO;
      hasdone = TRUE;
      undo->page = pg;
      undo->bg = (struct Background *)g_memdup(pg->bg, sizeof(struct Background));
      undo->bg->canvas_item = NULL;

      if (pg->bg->type != BG_SOLID) {
        pg->bg->type = BG_SOLID;
        pg->bg->color_no = COLOR_WHITE;
        pg->bg->color_rgba = predef_bgcolors_rgba[COLOR_WHITE];
        pg->bg->filename = NULL;
        pg->bg->pixbuf = NULL;
        must_upd = TRUE;
      }
      pg->bg->ruling = style;
      update_canvas_bg(pg);
    }
    if (!ui.bg_apply_all_pages) break;
  }
  if (hasdone) undo->multiop -= MULTIOP_CONT_REDO;
  if (must_upd) update_page_stuff();
}

gboolean ok_to_close(void)
{
  GtkWidget *dialog;
  GtkResponseType response;
  GList *pagelist;

  if (ui.saved) return TRUE;
  dialog = gtk_message_dialog_new(GTK_WINDOW (winMain), GTK_DIALOG_DESTROY_WITH_PARENT,
    GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "Save changes to '%s'?",
    (ui.filename!=NULL) ? ui.filename:"Untitled");
  gtk_dialog_add_button(GTK_DIALOG (dialog), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
  response = gtk_dialog_run(GTK_DIALOG (dialog));
  gtk_widget_destroy(dialog);
  if (response == GTK_RESPONSE_CANCEL || response == GTK_RESPONSE_DELETE_EVENT) 
    return FALSE; // aborted
  if (response == GTK_RESPONSE_YES) {
    on_fileSave_activate(NULL, NULL);
    if (!ui.saved) return FALSE; // if save failed, then we abort
  }
  return TRUE;
}

// test if we're still busy loading a PDF background file
gboolean page_ops_forbidden(void)
{
  return (bgpdf.status != STATUS_NOT_INIT && bgpdf.create_pages);
}

// selection / clipboard stuff

void reset_selection(void)
{
  if (ui.selection == NULL) return;
  if (ui.selection->canvas_item != NULL) 
    gtk_object_destroy(GTK_OBJECT(ui.selection->canvas_item));
  g_list_free(ui.selection->items);
  g_free(ui.selection);
  ui.selection = NULL;
  update_copy_paste_enabled();
  update_color_menu();
  update_thickness_buttons();
  update_color_buttons();
}

void move_journal_items_by(GList *itemlist, double dx, double dy,
                              struct Layer *l1, struct Layer *l2, GList *depths)
{
  struct Item *item;
  GnomeCanvasItem *refitem;
  GList *link;
  int i;
  double *pt;
  
  while (itemlist!=NULL) {
    item = (struct Item *)itemlist->data;
    if (item->type == ITEM_STROKE) {
      for (pt=item->path->coords, i=0; i<item->path->num_points; i++, pt+=2)
        { pt[0] += dx; pt[1] += dy; }
      item->bbox.left += dx;
      item->bbox.right += dx;
      item->bbox.top += dy;
      item->bbox.bottom += dy;
    }
    if (l1 != l2) {
      // find out where to insert
      if (depths != NULL) {
        if (depths->data == NULL) link = l2->items;
        else {
          link = g_list_find(l2->items, depths->data);
          if (link != NULL) link = link->next;
        }
      } else link = NULL;
      l2->items = g_list_insert_before(l2->items, link, item);
      l2->nitems++;
      l1->items = g_list_remove(l1->items, item);
      l1->nitems--;
    }
    if (depths != NULL) { // also raise/lower the canvas items
      if (item->canvas_item!=NULL) {
        if (depths->data == NULL) link = NULL;
        else link = g_list_find(l2->items, depths->data);
        if (link != NULL) refitem = ((struct Item *)(link->data))->canvas_item;
        else refitem = NULL;
        lower_canvas_item_to(l2->group, item->canvas_item, refitem);
      }
      depths = depths->next;
    }
    itemlist = itemlist->next;
  }
}

// Switch between button mappings

/* NOTE ABOUT BUTTON MAPPINGS: ui.cur_mapping is 0 except while a canvas
   click event is being processed ... */

void switch_mapping(int m)
{
  if (ui.cur_mapping == m) return;

  ui.cur_mapping = m;
  if (ui.toolno[m] < NUM_STROKE_TOOLS) 
    ui.cur_brush = &(ui.brushes[m][ui.toolno[m]]);
  update_tool_buttons();
  update_color_menu();
  update_cursor();
}

void process_mapping_activate(GtkMenuItem *menuitem, int m, int tool)
{
  if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem))) return;
  if (ui.cur_mapping!=0) return;
  if (ui.toolno[m] == tool) return;
  ui.toolno[m] = tool;
  ui.ruler[m] = FALSE;
  if (ui.linked_brush[m] == BRUSH_LINKED 
       && (tool==TOOL_PEN || tool==TOOL_HIGHLIGHTER))
    ui.ruler[m] = ui.ruler[0];
  if (ui.linked_brush[m] == BRUSH_COPIED) {
    ui.linked_brush[m] = BRUSH_STATIC;
    update_mappings_menu_linkings();
  }
}
