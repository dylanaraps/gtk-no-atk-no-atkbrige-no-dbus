#include <gtk/gtk.h>

#define ROWS 30

GSList *pending = NULL;
guint active = 0;

static void
got_files (GObject      *enumerate,
           GAsyncResult *res,
           gpointer      store);

static gboolean
start_enumerate (GListStore *store)
{
  GFileEnumerator *enumerate;
  GFile *file = g_object_get_data (G_OBJECT (store), "file");
  GError *error = NULL;

  enumerate = g_file_enumerate_children (file,
                                         G_FILE_ATTRIBUTE_STANDARD_TYPE
                                         "," G_FILE_ATTRIBUTE_STANDARD_ICON
                                         "," G_FILE_ATTRIBUTE_STANDARD_NAME
                                         "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                         0,
                                         NULL,
                                         &error);

  if (enumerate == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES) && active)
        {
          g_clear_error (&error);
          pending = g_slist_prepend (pending, g_object_ref (store));
          return TRUE;
        }

      g_clear_error (&error);
      g_object_unref (store);
      return FALSE;
    }

  if (active > 20)
    {
      g_object_unref (enumerate);
      pending = g_slist_prepend (pending, g_object_ref (store));
      return TRUE;
    }

  active++;
  g_file_enumerator_next_files_async (enumerate,
                                      g_file_is_native (file) ? 5000 : 100,
                                      G_PRIORITY_DEFAULT_IDLE,
                                      NULL,
                                      got_files,
                                      g_object_ref (store));

  g_object_unref (enumerate);
  return TRUE;
}

static void
got_files (GObject      *enumerate,
           GAsyncResult *res,
           gpointer      store)
{
  GList *l, *files;
  GFile *file = g_object_get_data (store, "file");
  GPtrArray *array;

  files = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (enumerate), res, NULL);
  if (files == NULL)
    {
      g_object_unref (store);
      if (pending)
        {
          GListStore *store = pending->data;
          pending = g_slist_remove (pending, store);
          start_enumerate (store);
        }
      active--;
      return;
    }

  array = g_ptr_array_new ();
  g_ptr_array_new_with_free_func (g_object_unref);
  for (l = files; l; l = l->next)
    {
      GFileInfo *info = l->data;
      GFile *child;

      child = g_file_get_child (file, g_file_info_get_name (info));
      g_object_set_data_full (G_OBJECT (info), "file", child, g_object_unref);
      g_ptr_array_add (array, info);
    }
  g_list_free (files);

  g_list_store_splice (store, g_list_model_get_n_items (store), 0, array->pdata, array->len);
  g_ptr_array_unref (array);

  g_file_enumerator_next_files_async (G_FILE_ENUMERATOR (enumerate),
                                      g_file_is_native (file) ? 5000 : 100,
                                      G_PRIORITY_DEFAULT_IDLE,
                                      NULL,
                                      got_files,
                                      store);
}

static int
compare_files (gconstpointer first,
               gconstpointer second,
               gpointer unused)
{
  GFile *first_file, *second_file;
  char *first_path, *second_path;
  int result;
#if 0
  GFileType first_type, second_type;

  /* This is a bit slow, because each g_file_query_file_type() does a stat() */
  first_type = g_file_query_file_type (G_FILE (first), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
  second_type = g_file_query_file_type (G_FILE (second), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);

  if (first_type == G_FILE_TYPE_DIRECTORY && second_type != G_FILE_TYPE_DIRECTORY)
    return -1;
  if (first_type != G_FILE_TYPE_DIRECTORY && second_type == G_FILE_TYPE_DIRECTORY)
    return 1;
#endif

  first_file = g_object_get_data (G_OBJECT (first), "file");
  second_file = g_object_get_data (G_OBJECT (second), "file");
  first_path = g_file_get_path (first_file);
  second_path = g_file_get_path (second_file);

  result = strcasecmp (first_path, second_path);

  g_free (first_path);
  g_free (second_path);

  return result;
}

static GListModel *
create_list_model_for_directory (gpointer file)
{
  GListStore *store;

  if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_DIRECTORY)
    return NULL;

  store = g_list_store_new (G_TYPE_FILE_INFO);
  g_object_set_data_full (G_OBJECT (store), "file", g_object_ref (file), g_object_unref);

  if (!start_enumerate (store))
    return NULL;

  return G_LIST_MODEL (store);
}

typedef struct _RowData RowData;
struct _RowData
{
  GtkWidget *depth_box;
  GtkWidget *expander;
  GtkWidget *icon;
  GtkWidget *name;

  GtkTreeListRow *current_item;
  GBinding *expander_binding;
};

static void row_data_notify_item (GtkListItem *item,
                                  GParamSpec  *pspec,
                                  RowData     *data);
static void
row_data_unbind (RowData *data)
{
  if (data->current_item == NULL)
    return;

  g_binding_unbind (data->expander_binding);

  g_clear_object (&data->current_item);
}

static void
row_data_bind (RowData        *data,
               GtkTreeListRow *item)
{
  GFileInfo *info;
  GIcon *icon;
  guint depth;

  row_data_unbind (data);

  if (item == NULL)
    return;

  data->current_item = g_object_ref (item);

  depth = gtk_tree_list_row_get_depth (item);
  gtk_widget_set_size_request (data->depth_box, 16 * depth, 0);

  gtk_widget_set_sensitive (data->expander, gtk_tree_list_row_is_expandable (item));
  data->expander_binding = g_object_bind_property (item, "expanded", data->expander, "active", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  info = gtk_tree_list_row_get_item (item);

  icon = g_file_info_get_icon (info);
  gtk_widget_set_visible (data->icon, icon != NULL);
  if (icon)
    gtk_image_set_from_gicon (GTK_IMAGE (data->icon), icon);

  gtk_label_set_label (GTK_LABEL (data->name), g_file_info_get_display_name (info));

  g_object_unref (info);
}

static void
row_data_notify_item (GtkListItem *item,
                      GParamSpec  *pspec,
                      RowData     *data)
{
  row_data_bind (data, gtk_list_item_get_item (item));
}

static void
row_data_free (gpointer _data)
{
  RowData *data = _data;

  row_data_unbind (data);

  g_slice_free (RowData, data);
}

static void
setup_widget (GtkListItem *list_item,
              gpointer     unused)
{
  GtkWidget *box, *child;
  RowData *data;

  data = g_slice_new0 (RowData);
  g_signal_connect (list_item, "notify::item", G_CALLBACK (row_data_notify_item), data);
  g_object_set_data_full (G_OBJECT (list_item), "row-data", data, row_data_free);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_container_add (GTK_CONTAINER (list_item), box);

  child = gtk_label_new (NULL);
  gtk_label_set_width_chars (GTK_LABEL (child), 5);
  gtk_label_set_xalign (GTK_LABEL (child), 1.0);
  g_object_bind_property (list_item, "position", child, "label", G_BINDING_SYNC_CREATE);
  gtk_container_add (GTK_CONTAINER (box), child);

  data->depth_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add (GTK_CONTAINER (box), data->depth_box);
  
  child = g_object_new (GTK_TYPE_BOX, "css-name", "expander", NULL);
  gtk_container_add (GTK_CONTAINER (box), child);
  data->expander = g_object_new (GTK_TYPE_TOGGLE_BUTTON, "css-name", "title", NULL);
  gtk_button_set_relief (GTK_BUTTON (data->expander), GTK_RELIEF_NONE);
  gtk_container_add (GTK_CONTAINER (child), data->expander);
  child = g_object_new (GTK_TYPE_SPINNER, "css-name", "arrow", NULL);
  gtk_container_add (GTK_CONTAINER (data->expander), child);

  data->icon = gtk_image_new ();
  gtk_container_add (GTK_CONTAINER (box), data->icon);

  data->name = gtk_label_new (NULL);
  gtk_container_add (GTK_CONTAINER (box), data->name);
}

static GListModel *
create_list_model_for_file_info (gpointer file_info,
                                 gpointer unused)
{
  GFile *file = g_object_get_data (file_info, "file");

  if (file == NULL)
    return NULL;

  return create_list_model_for_directory (file);
}

static gboolean
update_statusbar (GtkStatusbar *statusbar)
{
  GListModel *model = g_object_get_data (G_OBJECT (statusbar), "model");
  GString *string = g_string_new (NULL);
  guint n;
  gboolean result = G_SOURCE_REMOVE;

  gtk_statusbar_remove_all (statusbar, 0);

  n = g_list_model_get_n_items (model);
  g_string_append_printf (string, "%u", n);
  if (GTK_IS_FILTER_LIST_MODEL (model))
    {
      guint n_unfiltered = g_list_model_get_n_items (gtk_filter_list_model_get_model (GTK_FILTER_LIST_MODEL (model)));
      if (n != n_unfiltered)
        g_string_append_printf (string, "/%u", n_unfiltered);
    }
  g_string_append (string, " items");

  if (pending || active)
    {
      g_string_append_printf (string, " (%u directories remaining)", active + g_slist_length (pending));
      result = G_SOURCE_CONTINUE;
    }

  gtk_statusbar_push (statusbar, 0, string->str);
  g_free (string->str);

  return result;
}

static gboolean
match_file (gpointer item, gpointer data)
{
  GtkWidget *search_entry = data;
  GFileInfo *info = gtk_tree_list_row_get_item (item);
  GFile *file = g_object_get_data (G_OBJECT (info), "file");
  char *path;
  gboolean result;
  
  path = g_file_get_path (file);

  result = strstr (path, gtk_entry_get_text (GTK_ENTRY (search_entry))) != NULL;

  g_object_unref (info);
  g_free (path);

  return result;
}

static gboolean invert_sort;

static void
toggle_sort (GtkButton *button, GtkSortListModel *sort)
{
  invert_sort = !invert_sort;

  gtk_button_set_icon_name (button, invert_sort ? "view-sort-descending" : "view-sort-ascending");

  gtk_sort_list_model_resort (sort);
}

static int
sort_tree (gconstpointer a, gconstpointer b, gpointer data)
{
  GtkTreeListRow *ra = (GtkTreeListRow *) a;
  GtkTreeListRow *rb = (GtkTreeListRow *) b;
  GtkTreeListRow *pa, *pb;
  guint da, db;
  int i;
  GFile *ia, *ib;
  int cmp = 0;

  da = gtk_tree_list_row_get_depth (ra);
  db = gtk_tree_list_row_get_depth (rb);

  if (da > db)
    for (i = 0; i < da - db; i++)
      {
        ra = gtk_tree_list_row_get_parent (ra);
        if (ra) g_object_unref (ra);
      }
  if (db > da)
    for (i = 0; i < db - da; i++)
      {
        rb = gtk_tree_list_row_get_parent (rb);
        if (rb) g_object_unref (rb);
      }

  /* now ra and rb are ancestors of a and b at the same depth */

  if (ra == rb)
    return da - db;

  pa = ra;
  pb = rb;
  do {
    ra = pa;
    rb = pb;
    pa = gtk_tree_list_row_get_parent (ra);
    pb = gtk_tree_list_row_get_parent (rb);
    if (pa) g_object_unref (pa);
    if (pb) g_object_unref (pb);
  } while (pa != pb);

  /* now ra and rb are ancestors of a and b that have a common parent */

  ia = gtk_tree_list_row_get_item (ra);
  ib = gtk_tree_list_row_get_item (rb);

  cmp = compare_files (ia, ib, NULL);

  g_object_unref (ia);
  g_object_unref (ib);

  if (invert_sort)
    cmp = -cmp;

  return cmp;
}

int
main (int argc, char *argv[])
{
  GtkWidget *win, *vbox, *sw, *listview, *search_entry, *statusbar;
  GtkWidget *hbox, *button;
  GListModel *dirmodel;
  GtkTreeListModel *tree;
  GtkFilterListModel *filter;
  GtkSortListModel *sort;
  GtkSelectionModel *selection;
  GFile *root;

  gtk_init ();

  win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (win), 400, 600);
  g_signal_connect (win, "destroy", G_CALLBACK (gtk_main_quit), win);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add (GTK_CONTAINER (win), vbox);
  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add (GTK_CONTAINER (vbox), hbox);

  search_entry = gtk_search_entry_new ();
  gtk_container_add (GTK_CONTAINER (hbox), search_entry);
  gtk_widget_set_hexpand (search_entry, TRUE);
  button = gtk_button_new_from_icon_name ("view-sort-ascending");
  gtk_container_add (GTK_CONTAINER (hbox), button);

  sw = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_set_vexpand (sw, TRUE);
  gtk_search_entry_set_key_capture_widget (GTK_SEARCH_ENTRY (search_entry), sw);
  gtk_container_add (GTK_CONTAINER (vbox), sw);

  listview = gtk_list_view_new ();

  gtk_list_view_set_functions (GTK_LIST_VIEW (listview),
                               setup_widget,
                               NULL,
                               NULL, NULL);
  gtk_container_add (GTK_CONTAINER (sw), listview);

  if (argc > 1)
    root = g_file_new_for_commandline_arg (argv[1]);
  else
    root = g_file_new_for_path (g_get_current_dir ());
  dirmodel = create_list_model_for_directory (root);
  tree = gtk_tree_list_model_new (FALSE,
                                  dirmodel,
                                  TRUE,
                                  create_list_model_for_file_info,
                                  NULL, NULL);
  g_object_unref (dirmodel);
  g_object_unref (root);

  sort = gtk_sort_list_model_new (G_LIST_MODEL (tree), sort_tree, NULL, NULL);

  g_signal_connect (button, "clicked", G_CALLBACK (toggle_sort), sort);

  filter = gtk_filter_list_model_new (G_LIST_MODEL (sort),
                                      match_file,
                                      search_entry,
                                      NULL);
                                  
  g_signal_connect_swapped (search_entry, "search-changed", G_CALLBACK (gtk_filter_list_model_refilter), filter);
  selection = GTK_SELECTION_MODEL (gtk_single_selection_new (G_LIST_MODEL (filter)));

  gtk_list_view_set_model (GTK_LIST_VIEW (listview), G_LIST_MODEL (selection));

  statusbar = gtk_statusbar_new ();
  gtk_widget_add_tick_callback (statusbar, (GtkTickCallback) update_statusbar, NULL, NULL);
  g_object_set_data (G_OBJECT (statusbar), "model", filter);
  g_signal_connect_swapped (filter, "items-changed", G_CALLBACK (update_statusbar), statusbar);
  update_statusbar (GTK_STATUSBAR (statusbar));
  gtk_container_add (GTK_CONTAINER (vbox), statusbar);

  g_object_unref (tree);
  g_object_unref (filter);

  gtk_widget_show (win);

  gtk_main ();

  return 0;
}