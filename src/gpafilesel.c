/* GTK - The GIMP Toolkit
 * Copyright (C) 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Modified by the GTK+ Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GTK+ Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GTK+ at ftp://ftp.gtk.org/pub/gtk/. 
 */

#include "config.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#include <errno.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_WINSOCK_H
#include <winsock.h>		/* For gethostname */
#endif

#include "fnmatch.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include "gpafilesel.h"
#include "i18n.h"

#if defined(G_OS_WIN32) || defined(G_WITH_CYGWIN)
#define STRICT
#include <windows.h>

#ifdef G_OS_WIN32
#include <direct.h>
#include <io.h>
#define mkdir(p,m) _mkdir(p)
#ifndef S_ISDIR
#define S_ISDIR(mode) ((mode)&_S_IFDIR)
#endif

#endif /* G_OS_WIN32 */

#endif /* G_OS_WIN32 || G_WITH_CYGWIN */

#define DIR_LIST_WIDTH   180
#define DIR_LIST_HEIGHT  180
#define FILE_LIST_WIDTH  180
#define FILE_LIST_HEIGHT 180

/* The Hurd doesn't define either PATH_MAX or MAXPATHLEN, so we put this
 * in here, since the rest of the code in the file does require some
 * fixed maximum.
 */
#ifndef MAXPATHLEN
#  ifdef PATH_MAX
#    define MAXPATHLEN PATH_MAX
#  else
#    define MAXPATHLEN 2048
#  endif
#endif

/* I've put this here so it doesn't get confused with the 
 * file completion interface */
typedef struct _HistoryCallbackArg HistoryCallbackArg;

struct _HistoryCallbackArg
{
  gchar *directory;
  GtkWidget *menu_item;
};


typedef struct _CompletionState    CompletionState;
typedef struct _CompletionDir      CompletionDir;
typedef struct _CompletionDirSent  CompletionDirSent;
typedef struct _CompletionDirEntry CompletionDirEntry;
typedef struct _CompletionUserDir  CompletionUserDir;
typedef struct _PossibleCompletion PossibleCompletion;

/* Non-external file completion decls and structures */

/* A contant telling PRCS how many directories to cache.  Its actually
 * kept in a list, so the geometry isn't important. */
#define CMPL_DIRECTORY_CACHE_SIZE 10

/* A constant used to determine whether a substring was an exact
 * match by first_diff_index()
 */
#define PATTERN_MATCH -1
/* The arguments used by all fnmatch() calls below
 */
#define FNMATCH_FLAGS (FNM_PATHNAME | FNM_PERIOD)

#define CMPL_ERRNO_TOO_LONG ((1<<16)-1)
#define CMPL_ERRNO_DID_NOT_CONVERT ((1<<16)-2)

/* This structure contains all the useful information about a directory
 * for the purposes of filename completion.  These structures are cached
 * in the CompletionState struct.  CompletionDir's are reference counted.
 */
struct _CompletionDirSent
{
  ino_t inode;
  time_t mtime;
  dev_t device;

  gint entry_count;
  struct _CompletionDirEntry *entries;
};

struct _CompletionDir
{
  CompletionDirSent *sent;

  gchar *fullname;
  gint fullname_len;

  struct _CompletionDir *cmpl_parent;
  gint cmpl_index;
  gchar *cmpl_text;
};

/* This structure contains pairs of directory entry names with a flag saying
 * whether or not they are a valid directory.  NOTE: This information is used
 * to provide the caller with information about whether to update its completions
 * or try to open a file.  Since directories are cached by the directory mtime,
 * a symlink which points to an invalid file (which will not be a directory),
 * will not be reevaluated if that file is created, unless the containing
 * directory is touched.  I consider this case to be worth ignoring (josh).
 */
struct _CompletionDirEntry
{
  gboolean is_dir;
  gchar *entry_name;
};

struct _CompletionUserDir
{
  gchar *login;
  gchar *homedir;
};

struct _PossibleCompletion
{
  /* accessible fields, all are accessed externally by functions
   * declared above
   */
  gchar *text;
  gint is_a_completion;
  gboolean is_directory;

  /* Private fields
   */
  gint text_alloc;
};

struct _CompletionState
{
  gint last_valid_char;
  gchar *updated_text;
  gint updated_text_len;
  gint updated_text_alloc;
  gboolean re_complete;

  gchar *user_dir_name_buffer;
  gint user_directories_len;

  gchar *last_completion_text;

  gint user_completion_index; /* if >= 0, currently completing ~user */

  struct _CompletionDir *completion_dir; /* directory completing from */
  struct _CompletionDir *active_completion_dir;

  struct _PossibleCompletion the_completion;

  struct _CompletionDir *reference_dir; /* initial directory */

  GList* directory_storage;
  GList* directory_sent_storage;

  struct _CompletionUserDir *user_directories;
};

enum {
  PROP_0,
  PROP_SHOW_FILEOPS,
  PROP_FILENAME
};

/* Pixmaps.
 */
static GdkPixmap *pm_folder = NULL;
static GdkPixmap *mask_folder = NULL;
static GdkPixmap *pm_open_folder = NULL;
static GdkPixmap *mask_open_folder = NULL;
static GdkPixmap *pm_floppy = NULL;
static GdkPixmap *mask_floppy = NULL;
static GdkPixmap *pm_harddisk = NULL;
static GdkPixmap *mask_harddisk = NULL;
static GdkColor transparent = { 0 };

/* File completion functions which would be external, were they used
 * outside of this file.
 */

static CompletionState*    cmpl_init_state        (void);
static void                cmpl_free_state        (CompletionState *cmpl_state);
static gint                cmpl_state_okay        (CompletionState* cmpl_state);
static const gchar*        cmpl_strerror          (gint);

static PossibleCompletion* cmpl_completion_matches(gchar           *text_to_complete,
						   gchar          **remaining_text,
						   CompletionState *cmpl_state);

/* Returns a name for consideration, possibly a completion, this name
 * will be invalid after the next call to cmpl_next_completion.
 */
static char*               cmpl_this_completion   (PossibleCompletion*);

/* True if this completion matches the given text.  Otherwise, this
 * output can be used to have a list of non-completions.
 */
static gint                cmpl_is_a_completion   (PossibleCompletion*);

/* True if the completion is a directory
 */
static gboolean            cmpl_is_directory      (PossibleCompletion*);

/* Obtains the next completion, or NULL
 */
static PossibleCompletion* cmpl_next_completion   (CompletionState*);

/* Updating completions: the return value of cmpl_updated_text() will
 * be text_to_complete completed as much as possible after the most
 * recent call to cmpl_completion_matches.  For the present
 * application, this is the suggested replacement for the user's input
 * string.  You must CALL THIS AFTER ALL cmpl_text_completions have
 * been received.
 */
static gchar*              cmpl_updated_text       (CompletionState* cmpl_state);

/* After updating, to see if the completion was a directory, call
 * this.  If it was, you should consider re-calling completion_matches.
 */
static gboolean            cmpl_updated_dir        (CompletionState* cmpl_state);

/* Current location: if using file completion, return the current
 * directory, from which file completion begins.  More specifically,
 * the cwd concatenated with all exact completions up to the last
 * directory delimiter('/').
 */
static gchar*              cmpl_reference_position (CompletionState* cmpl_state);

/* backing up: if cmpl_completion_matches returns NULL, you may query
 * the index of the last completable character into cmpl_updated_text.
 */
static gint                cmpl_last_valid_char    (CompletionState* cmpl_state);

/* When the user selects a non-directory, call cmpl_completion_fullname
 * to get the full name of the selected file.
 */
static gchar*              cmpl_completion_fullname (const gchar*, CompletionState* cmpl_state);


/* Directory operations. */
static CompletionDir* open_ref_dir         (gchar* text_to_complete,
					    gchar** remaining_text,
					    CompletionState* cmpl_state);
#if !defined(G_OS_WIN32) && !defined(G_WITH_CYGWIN)
static gboolean       check_dir            (gchar *dir_name, 
					    struct stat *result, 
					    gboolean *stat_subdirs);
#endif
static CompletionDir* open_dir             (gchar* dir_name,
					    CompletionState* cmpl_state);
#ifdef HAVE_PWD_H
static CompletionDir* open_user_dir        (const gchar* text_to_complete,
					    CompletionState *cmpl_state);
#endif
static CompletionDir* open_relative_dir    (gchar* dir_name, CompletionDir* dir,
					    CompletionState *cmpl_state);
static CompletionDirSent* open_new_dir     (gchar* dir_name, 
					    struct stat* sbuf,
					    gboolean stat_subdirs);
static gint           correct_dir_fullname (CompletionDir* cmpl_dir);
static gint           correct_parent       (CompletionDir* cmpl_dir,
					    struct stat *sbuf);
static gchar*         find_parent_dir_fullname    (gchar* dirname);
static CompletionDir* attach_dir           (CompletionDirSent* sent,
					    gchar* dir_name,
					    CompletionState *cmpl_state);
static void           free_dir_sent (CompletionDirSent* sent);
static void           free_dir      (CompletionDir  *dir);
static void           prune_memory_usage(CompletionState *cmpl_state);

/* Completion operations */
#ifdef HAVE_PWD_H
static PossibleCompletion* attempt_homedir_completion(gchar* text_to_complete,
						      CompletionState *cmpl_state);
#endif
static PossibleCompletion* attempt_file_completion(CompletionState *cmpl_state);
static CompletionDir* find_completion_dir(gchar* text_to_complete,
					  gchar** remaining_text,
					  CompletionState* cmpl_state);
static PossibleCompletion* append_completion_text(gchar* text,
						  CompletionState* cmpl_state);
#ifdef HAVE_PWD_H
static gint get_pwdb(CompletionState* cmpl_state);
static gint compare_user_dir(const void* a, const void* b);
#endif
static gint first_diff_index(gchar* pat, gchar* text);
static gint compare_cmpl_dir(const void* a, const void* b);
static void update_cmpl(PossibleCompletion* poss,
			CompletionState* cmpl_state);

static void read_directory (GtkCTree *dir_list, GtkCTreeNode *parent,
                            gchar *path, gint level);
static void directory_expand (GtkCTree *dir_list, gpointer data);

static void gpa_file_selection_class_init    (GpaFileSelectionClass *klass);
static void gpa_file_selection_set_property  (GObject         *object,
					      guint            prop_id,
					      const GValue    *value,
					      GParamSpec      *pspec);
static void gpa_file_selection_get_property  (GObject         *object,
					      guint            prop_id,
					      GValue          *value,
					      GParamSpec      *pspec);
static void gpa_file_selection_init          (GpaFileSelection      *filesel);
static void gpa_file_selection_finalize      (GObject               *object);
static void gpa_file_selection_destroy       (GtkObject             *object);
static gint gpa_file_selection_key_press     (GtkWidget             *widget,
					      GdkEventKey           *event,
					      gpointer               user_data);
static gint gpa_file_selection_insert_text   (GtkWidget             *widget,
					      const gchar           *new_text,
					      gint                   new_text_length,
					      gint                  *position,
					      gpointer               user_data);

static void gpa_file_selection_file_button (GtkWidget *widget,
					    gint row, 
					    gint column, 
					    GdkEventButton *bevent,
					    gpointer user_data);

static void gpa_file_selection_dir_button (GtkWidget *widget,
					   gint row, 
					   gint column, 
					   GdkEventButton *bevent,
					   gpointer data);

static void gpa_file_selection_populate (GpaFileSelection *fs, gchar *rel_path,
					 gint try_complete);
static void gpa_file_selection_abort (GpaFileSelection *fs);

static void gpa_file_selection_update_history_menu (GpaFileSelection       *fs,
						    gchar                  *current_dir);

static void gpa_file_selection_create_dir (GtkWidget *widget, gpointer data);
static void gpa_file_selection_delete_file (GtkWidget *widget, gpointer data);
static void gpa_file_selection_rename_file (GtkWidget *widget, gpointer data);



static GtkWindowClass *parent_class = NULL;

/* Saves errno when something cmpl does fails. */
static gint cmpl_errno;

#ifdef G_WITH_CYGWIN
/*
 * Take the path currently in the file selection
 * entry field and translate as necessary from
 * a WIN32 style to CYGWIN32 style path.  For
 * instance translate:
 * x:\somepath\file.jpg
 * to:
 * //x/somepath/file.jpg
 *
 * Replace the path in the selection text field.
 * Return a boolean value concerning whether a
 * translation had to be made.
 */
int
translate_win32_path (GpaFileSelection *filesel)
{
  int updated = 0;
  gchar *path;

  /*
   * Retrieve the current path
   */
  path = gtk_entry_get_text (GTK_ENTRY (filesel->selection_entry));

  /*
   * Translate only if this looks like a DOS-ish
   * path... First handle any drive letters.
   */
  if (isalpha (path[0]) && (path[1] == ':')) {
    /*
     * This part kind of stinks... It isn't possible
     * to know if there is enough space in the current
     * string for the extra character required in this
     * conversion.  Assume that there isn't enough space
     * and use the set function on the text field to
     * set the newly created string.
     */
    gchar *newPath = g_strdup_printf ("//%c/%s", path[0], (path + 3));
    gtk_entry_set_text (GTK_ENTRY (filesel->selection_entry), newPath);

    path = newPath;
    updated = 1;
  }

  /*
   * Now, replace backslashes with forward slashes 
   * if necessary.
   */
  if (strchr (path, '\\'))
    {
      int index;
      for (index = 0; path[index] != '\0'; index++)
	if (path[index] == '\\')
	  path[index] = '/';
      
      updated = 1;
    }
    
  return updated;
}
#endif

#include "folder.xpm"
#include "open_folder.xpm"
#include "floppy.xpm"
#include "harddisk.xpm"

static void
read_directory (GtkCTree *dir_list, GtkCTreeNode *parent,
                gchar *path, gint level)
{
  gchar *text[1];
  GtkCTreeNode *this_node;
  GDir *directory;
  const gchar *dirent;

  text[0] = path + strlen (path) - 1;
  while (text[0] > path && *text[0] != '/')
    text[0]--;
  if (*text[0] == '/')
    text[0]++;

  if (strcmp (path, "/") == 0)
    this_node = NULL;
  else if (strcmp (path, "A:") == 0 || strcmp (path, "B:") == 0)
    this_node = gtk_ctree_insert_node (dir_list, parent, NULL, text, 5,
                                       pm_floppy, mask_floppy,
                                       pm_floppy, mask_floppy,
                                       FALSE, FALSE);
  else if (path[1] == ':' && path[2] == 0)
    this_node = gtk_ctree_insert_node (dir_list, parent, NULL, text, 5,
                                       pm_harddisk, mask_harddisk,
                                       pm_harddisk, mask_harddisk,
                                       FALSE, FALSE);
  else
    this_node = gtk_ctree_insert_node (dir_list, parent, NULL, text, 5,
                                       pm_folder, mask_folder,
                                       pm_open_folder, mask_open_folder,
                                       FALSE, FALSE);
  directory = g_dir_open (path, 0, NULL);
  if (directory)
    {
      while ((dirent = g_dir_read_name (directory)) != NULL)
        {
          gchar *full_dir_name = g_strconcat (path, "/", dirent, NULL);
          if (g_file_test (full_dir_name, G_FILE_TEST_IS_DIR))
            {
              text[0] = (gchar *) dirent;
              if (level < 3)
                read_directory (dir_list, this_node, full_dir_name, level + 1);
              else
                gtk_ctree_insert_node (dir_list, this_node, NULL, text, 5,
                                       pm_folder, mask_folder,
                                       pm_open_folder, mask_open_folder,
                                       FALSE, FALSE);
            }
          g_free (full_dir_name);
        }
      g_dir_close (directory);
    }
}

static void
directory_expand (GtkCTree *dir_list, gpointer data)
{
}

GtkType
gpa_file_selection_get_type (void)
{
  static GtkType file_selection_type = 0;

  if (!file_selection_type)
    {
      static const GtkTypeInfo filesel_info =
      {
	"GpaFileSelection",
	sizeof (GpaFileSelection),
	sizeof (GpaFileSelectionClass),
	(GtkClassInitFunc) gpa_file_selection_class_init,
	(GtkObjectInitFunc) gpa_file_selection_init,
	/* reserved_1 */ NULL,
	/* reserved_2 */ NULL,
        (GtkClassInitFunc) NULL,
      };

      file_selection_type = gtk_type_unique (GTK_TYPE_DIALOG, &filesel_info);
    }

  return file_selection_type;
}

static void
gpa_file_selection_class_init (GpaFileSelectionClass *class)
{
  GObjectClass *gobject_class;
  GtkObjectClass *object_class;

  gobject_class = (GObjectClass*) class;
  object_class = (GtkObjectClass*) class;

  parent_class = gtk_type_class (GTK_TYPE_DIALOG);

  gobject_class->finalize = gpa_file_selection_finalize;
  gobject_class->set_property = gpa_file_selection_set_property;
  gobject_class->get_property = gpa_file_selection_get_property;
   
  g_object_class_install_property (gobject_class,
                                   PROP_FILENAME,
                                   g_param_spec_string ("filename",
                                                        _("Filename"),
                                                        _("The currently selected filename."),
                                                        NULL,
                                                        G_PARAM_READABLE | G_PARAM_WRITABLE));
  g_object_class_install_property (gobject_class,
				   PROP_SHOW_FILEOPS,
				   g_param_spec_boolean ("show_fileops",
							 _("Show file operations"),
							 _("Whether buttons for creating/manipulating files should be displayed."),
							 FALSE,
							 G_PARAM_READABLE |
							 G_PARAM_WRITABLE));
  object_class->destroy = gpa_file_selection_destroy;
}

static void gpa_file_selection_set_property (GObject         *object,
					     guint            prop_id,
					     const GValue    *value,
					     GParamSpec      *pspec)
{
  GpaFileSelection *filesel;

  filesel = GPA_FILE_SELECTION (object);

  switch (prop_id)
    {
    case PROP_FILENAME:
      gpa_file_selection_set_filename (filesel,
                                       g_value_get_string (value));
      break;
      
    case PROP_SHOW_FILEOPS:
      if (g_value_get_boolean (value))
	 gpa_file_selection_show_fileop_buttons (filesel);
      else
	 gpa_file_selection_hide_fileop_buttons (filesel);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void gpa_file_selection_get_property (GObject         *object,
					     guint            prop_id,
					     GValue          *value,
					     GParamSpec      *pspec)
{
  GpaFileSelection *filesel;

  filesel = GPA_FILE_SELECTION (object);

  switch (prop_id)
    {
    case PROP_FILENAME:
      g_value_set_string (value,
                          gpa_file_selection_get_filename(filesel));
      break;

    case PROP_SHOW_FILEOPS:
      /* This is a little bit hacky, but doing otherwise would require
       * adding a field to the object.
       */
      g_value_set_boolean (value, (filesel->fileop_c_dir && 
				   filesel->fileop_del_file &&
				   filesel->fileop_ren_file));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static gboolean
grab_default (GtkWidget *widget)
{
  gtk_widget_grab_default (widget);
  return FALSE;
}

static void
gtk_button_correct_label (GtkButton *button, gchar *label_text)
{
  GtkStockItem item;
  GtkWidget *label;
  GtkWidget *image;
  GtkWidget *hbox;
  GtkWidget *align;

  if (!button->constructed)
    return;
  
  if (button->label_text == NULL)
    return;

  if (GTK_BIN (button)->child)
    gtk_container_remove (GTK_CONTAINER (button),
			  GTK_BIN (button)->child);

  
  if (gtk_stock_lookup (button->label_text, &item))
    {
      label = gtk_label_new_with_mnemonic (label_text);

      gtk_label_set_mnemonic_widget (GTK_LABEL (label), GTK_WIDGET (button));
      
      image = gtk_image_new_from_stock (button->label_text, GTK_ICON_SIZE_BUTTON);
      hbox = gtk_hbox_new (FALSE, 2);

      align = gtk_alignment_new (0.5, 0.5, 0.0, 0.0);
      
      gtk_box_pack_start (GTK_BOX (hbox), image, FALSE, FALSE, 0);
      gtk_box_pack_end (GTK_BOX (hbox), label, FALSE, FALSE, 0);
      
      gtk_container_add (GTK_CONTAINER (button), align);
      gtk_container_add (GTK_CONTAINER (align), hbox);
      gtk_widget_show_all (align);

      return;
    }

  if (button->use_underline)
    {
      label = gtk_label_new_with_mnemonic (button->label_text);
      gtk_label_set_mnemonic_widget (GTK_LABEL (label), GTK_WIDGET (button));
    }
  else
    label = gtk_label_new (button->label_text);
  
  gtk_misc_set_alignment (GTK_MISC (label), 0.5, 0.5);

  gtk_widget_show (label);
  gtk_container_add (GTK_CONTAINER (button), label);
}
     
static void
gpa_file_selection_init (GpaFileSelection *filesel)
{
  GtkWidget *entry_vbox;
  GtkWidget *label;
  GtkWidget *list_hbox;
  GtkWidget *confirm_area;
  GtkWidget *pulldown_hbox;
  GtkWidget *scrolled_win;
  GtkWidget *eventbox;
  GtkDialog *dialog;
#ifdef HAVE_DRIVE_LETTERS
  gchar buffer[128], *drive;
#endif /* HAVE_DRIVE_LETTERS */
  
  char *dir_title [2];
  char *file_title [2];

  dialog = GTK_DIALOG (filesel);

  filesel->cmpl_state = cmpl_init_state ();

  /* The dialog-sized vertical box  */
  filesel->main_vbox = dialog->vbox;
  gtk_container_set_border_width (GTK_CONTAINER (filesel), 10);

  /* The horizontal box containing create, rename etc. buttons */
  filesel->button_area = gtk_hbutton_box_new ();
  gtk_button_box_set_layout (GTK_BUTTON_BOX (filesel->button_area), GTK_BUTTONBOX_START);
  gtk_box_set_spacing (GTK_BOX (filesel->button_area), 0);
  gtk_box_pack_start (GTK_BOX (filesel->main_vbox), filesel->button_area, 
		      FALSE, FALSE, 0);
  gtk_widget_show (filesel->button_area);
  
  gpa_file_selection_show_fileop_buttons (filesel);

  /* hbox for pulldown menu */
  pulldown_hbox = gtk_hbox_new (TRUE, 5);
  gtk_box_pack_start (GTK_BOX (filesel->main_vbox), pulldown_hbox, FALSE, FALSE, 0);
  gtk_widget_show (pulldown_hbox);
  
  /* Pulldown menu */
  filesel->history_pulldown = gtk_option_menu_new ();
  gtk_widget_show (filesel->history_pulldown);
  gtk_box_pack_start (GTK_BOX (pulldown_hbox), filesel->history_pulldown, 
		      FALSE, FALSE, 0);
    
  /*  The horizontal box containing the directory and file listboxes  */
  list_hbox = gtk_hbox_new (FALSE, 5);
  gtk_box_pack_start (GTK_BOX (filesel->main_vbox), list_hbox, TRUE, TRUE, 0);
  gtk_widget_show (list_hbox);

  /* The directories ctree */
  dir_title[0] = _("Directories");
  dir_title[1] = NULL;
  filesel->dir_list = gtk_ctree_new_with_titles (1, 0, (gchar**) dir_title);
  gtk_widget_set_usize (filesel->dir_list, DIR_LIST_WIDTH, DIR_LIST_HEIGHT);
  gtk_signal_connect (GTK_OBJECT (filesel->dir_list), "select_row",
		      (GtkSignalFunc) gpa_file_selection_dir_button, 
		      (gpointer) filesel);
  gtk_clist_set_column_auto_resize (GTK_CLIST (filesel->dir_list), 0, TRUE);
  gtk_clist_column_titles_passive (GTK_CLIST (filesel->dir_list));
  gtk_ctree_set_line_style (GTK_CTREE (filesel->dir_list), GTK_CTREE_LINES_SOLID);
  gtk_ctree_set_expander_style (GTK_CTREE (filesel->dir_list), GTK_CTREE_EXPANDER_SQUARE);
  if (GTK_CLIST (filesel->dir_list)->row_height < 16)
    gtk_clist_set_row_height (GTK_CLIST (filesel->dir_list), 16);

  gtk_signal_connect (GTK_OBJECT (filesel->dir_list), "tree_expand",
                      (GtkSignalFunc) directory_expand, NULL);

  scrolled_win = gtk_scrolled_window_new (NULL, NULL);
  gtk_container_add (GTK_CONTAINER (scrolled_win), filesel->dir_list);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_win),
				  GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  gtk_container_set_border_width (GTK_CONTAINER (scrolled_win), 5);
  gtk_box_pack_start (GTK_BOX (list_hbox), scrolled_win, TRUE, TRUE, 0);
  gtk_widget_show (filesel->dir_list);
  gtk_widget_show (scrolled_win);

  gtk_widget_realize (GTK_WIDGET (filesel));  /* @@@??? */
  pm_folder = gdk_pixmap_create_from_xpm_d (GTK_WIDGET (filesel)->window,
                                            &mask_folder, &transparent,
                                            folder_xpm);
  pm_open_folder = gdk_pixmap_create_from_xpm_d (GTK_WIDGET (filesel)->window,
                                                 &mask_open_folder, &transparent,
                                                 open_folder_xpm);
  pm_floppy = gdk_pixmap_create_from_xpm_d (GTK_WIDGET (filesel)->window,
                                            &mask_floppy, &transparent,
                                            floppy_xpm);
  pm_harddisk = gdk_pixmap_create_from_xpm_d (GTK_WIDGET (filesel)->window,
                                              &mask_harddisk, &transparent,
                                              harddisk_xpm);

  /* The files ctree */
  file_title[0] = _("Files");
  file_title[1] = NULL;
  filesel->file_list = gtk_clist_new_with_titles (1, (gchar**) file_title);
  gtk_widget_set_usize (filesel->file_list, FILE_LIST_WIDTH, FILE_LIST_HEIGHT);
  gtk_signal_connect (GTK_OBJECT (filesel->file_list), "select_row",
		      (GtkSignalFunc) gpa_file_selection_file_button, 
		      (gpointer) filesel);
  gtk_clist_set_column_auto_resize (GTK_CLIST (filesel->file_list), 0, TRUE);
  gtk_clist_column_titles_passive (GTK_CLIST (filesel->file_list));

  scrolled_win = gtk_scrolled_window_new (NULL, NULL);
  gtk_container_add (GTK_CONTAINER (scrolled_win), filesel->file_list);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_win),
				  GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  gtk_container_set_border_width (GTK_CONTAINER (scrolled_win), 5);
  gtk_box_pack_start (GTK_BOX (list_hbox), scrolled_win, TRUE, TRUE, 0);
  gtk_widget_show (filesel->file_list);
  gtk_widget_show (scrolled_win);

  /* action area for packing buttons into. */
  filesel->action_area = gtk_hbox_new (TRUE, 0);
  gtk_box_pack_start (GTK_BOX (filesel->main_vbox), filesel->action_area, 
		      FALSE, FALSE, 0);
  gtk_widget_show (filesel->action_area);
  
  /*  The OK/Cancel button area */
  confirm_area = dialog->action_area;

  /*  The Cancel button  */
  filesel->cancel_button = gtk_dialog_add_button (dialog,
                                                  GTK_STOCK_CANCEL,
                                                  GTK_RESPONSE_CANCEL);
  gtk_button_correct_label (GTK_BUTTON (filesel->cancel_button), _("_Cancel"));

  /*  The OK button  */
  filesel->ok_button = gtk_dialog_add_button (dialog,
                                              GTK_STOCK_OK,
                                              GTK_RESPONSE_OK);
  gtk_button_correct_label (GTK_BUTTON (filesel->ok_button), _("_Open"));
  
  gtk_widget_grab_default (filesel->ok_button);

  /*  The selection entry widget  */
  entry_vbox = gtk_vbox_new (FALSE, 2);
  gtk_box_pack_end (GTK_BOX (filesel->main_vbox), entry_vbox, FALSE, FALSE, 2);
  gtk_widget_show (entry_vbox);
  
  eventbox = gtk_event_box_new ();
  filesel->selection_text = label = gtk_label_new ("");
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
  gtk_container_add (GTK_CONTAINER (eventbox), label);
  gtk_box_pack_start (GTK_BOX (entry_vbox), eventbox, FALSE, FALSE, 0);
  gtk_widget_show (label);
  gtk_widget_show (eventbox);

  filesel->selection_entry = gtk_entry_new ();
  gtk_signal_connect (GTK_OBJECT (filesel->selection_entry), "key_press_event",
		      (GtkSignalFunc) gpa_file_selection_key_press, filesel);
  gtk_signal_connect (GTK_OBJECT (filesel->selection_entry), "insert_text",
		      (GtkSignalFunc) gpa_file_selection_insert_text, NULL);
  gtk_signal_connect_object (GTK_OBJECT (filesel->selection_entry), "focus_in_event",
			     (GtkSignalFunc) grab_default,
			     GTK_OBJECT (filesel->ok_button));
  gtk_signal_connect_object (GTK_OBJECT (filesel->selection_entry), "activate",
                             (GtkSignalFunc) gtk_button_clicked,
                             GTK_OBJECT (filesel->ok_button));
  gtk_box_pack_start (GTK_BOX (entry_vbox), filesel->selection_entry, TRUE, TRUE, 0);
  gtk_widget_show (filesel->selection_entry);

#ifdef HAVE_DRIVE_LETTERS
  GetLogicalDriveStrings (sizeof (buffer), buffer);
  drive = buffer;
  while (*drive)
    {
      int len = strlen (drive);
      drive[len - 1] = 0;
      drive[0] = toupper (drive[0]);
      read_directory (GTK_CTREE (filesel->dir_list), NULL, drive, 1);
      drive += (len + 1);
    }
#else /* not HAVE_DRIVE_LETTERS */
  read_directory (GTK_CTREE (filesel->dir_list), NULL, "/", 0);
#endif /* not HAVE_DRIVE_LETTERS */

  if (!cmpl_state_okay (filesel->cmpl_state))
    {
      gchar err_buf[256];

      sprintf (err_buf, _("Directory unreadable: %s"), cmpl_strerror (cmpl_errno));

      gtk_label_set_text (GTK_LABEL (filesel->selection_text), err_buf);
    }
  else
    {
      gpa_file_selection_populate (filesel, "", FALSE);
    }

  gtk_widget_grab_focus (filesel->selection_entry);
}

static gchar *
uri_list_extract_first_uri (const gchar* uri_list)
{
  const gchar *p, *q;
  
  g_return_val_if_fail (uri_list != NULL, NULL);
  
  p = uri_list;
  /* We don't actually try to validate the URI according to RFC
   * 2396, or even check for allowed characters - we just ignore
   * comments and trim whitespace off the ends.  We also
   * allow LF delimination as well as the specified CRLF.
   *
   * We do allow comments like specified in RFC 2483.
   */
  while (p)
    {
      if (*p != '#')
	{
	  while (g_ascii_isspace(*p))
	    p++;
	  
	  q = p;
	  while (*q && (*q != '\n') && (*q != '\r'))
	    q++;
	  
	  if (q > p)
	    {
	      q--;
	      while (q > p && g_ascii_isspace (*q))
		q--;

	      if (q > p)
		return g_strndup (p, q - p + 1);
	    }
	}
      p = strchr (p, '\n');
      if (p)
	p++;
    }
  return NULL;
}

static void
dnd_really_drop  (GtkWidget *dialog, gint response_id, GpaFileSelection *fs)
{
  gchar *filename;
  
  if (response_id == GTK_RESPONSE_YES)
    {
      filename = g_object_get_data (G_OBJECT (dialog), "gtk-fs-dnd-filename");

      gpa_file_selection_set_filename (fs, filename);
    }
  
  gtk_widget_destroy (dialog);
}


static void
filenames_dropped (GtkWidget        *widget,
		   GdkDragContext   *context,
		   gint              x,
		   gint              y,
		   GtkSelectionData *selection_data,
		   guint             info,
		   guint             time)
{
  char *uri = NULL;
  char *filename = NULL;
  char *hostname;
  char this_hostname[257];
  int res;
  GError *error = NULL;
	
  if (!selection_data->data)
    return;

  uri = uri_list_extract_first_uri ((char *)selection_data->data);
  
  if (!uri)
    return;

  filename = g_filename_from_uri (uri, &hostname, &error);
  g_free (uri);
  
  if (!filename)
    {
      g_warning ("Error getting dropped filename: %s\n",
		 error->message);
      g_error_free (error);
      return;
    }

  res = gethostname (this_hostname, 256);
  this_hostname[256] = 0;
  
  if ((hostname == NULL) ||
      (res == 0 && strcmp (hostname, this_hostname) == 0) ||
      (strcmp (hostname, "localhost") == 0))
    gpa_file_selection_set_filename (GPA_FILE_SELECTION (widget),
				     filename);
  else
    {
      GtkWidget *dialog;
      
      dialog = gtk_message_dialog_new (GTK_WINDOW (widget),
				       GTK_DIALOG_DESTROY_WITH_PARENT,
				       GTK_MESSAGE_QUESTION,
				       GTK_BUTTONS_YES_NO,
				       _("The file \"%s\" resides on another machine (called %s) and may not be availible to this program.\n"
					 "Are you sure that you want to select it?"), filename, hostname);

      g_object_set_data_full (G_OBJECT (dialog), "gtk-fs-dnd-filename", g_strdup (filename), g_free);
      
      g_signal_connect_data (dialog, "response",
			     (GCallback) dnd_really_drop, 
			     widget, NULL, 0);
      
      gtk_widget_show (dialog);
    }

  g_free (hostname);
  g_free (filename);
}

enum
{
  TARGET_URILIST,
  TARGET_UTF8_STRING,
  TARGET_STRING,
  TARGET_TEXT,
  TARGET_COMPOUND_TEXT
};


static void
filenames_drag_get (GtkWidget        *widget,
		    GdkDragContext   *context,
		    GtkSelectionData *selection_data,
		    guint             info,
		    guint             time,
		    GpaFileSelection *filesel)
{
  const gchar *file;
  gchar *uri_list;
  char hostname[256];
  int res;
  GError *error;

  file = gpa_file_selection_get_filename (filesel);

  if (file)
    {
      if (info == TARGET_URILIST)
	{
	  res = gethostname (hostname, 256);
	  
	  error = NULL;
	  uri_list = g_filename_to_uri (file, (!res)?hostname:NULL, &error);
	  if (!uri_list)
	    {
	      g_warning ("Error getting filename: %s\n",
			 error->message);
	      g_error_free (error);
	      return;
	    }
	  
	  gtk_selection_data_set (selection_data,
				  selection_data->target, 8,
				  (void *)uri_list, strlen((char *)uri_list));
	  g_free (uri_list);
	}
      else
	{
	  g_print ("Setting text: '%s'\n", file);
	  gtk_selection_data_set_text (selection_data, file, -1);
	}
    }
}

static void
file_selection_setup_dnd (GpaFileSelection *filesel)
{
  GtkWidget *eventbox;
  static GtkTargetEntry drop_types[] = {
    { "text/uri-list", 0, TARGET_URILIST}
  };
  static gint n_drop_types = sizeof(drop_types)/sizeof(drop_types[0]);
  static GtkTargetEntry drag_types[] = {
    { "text/uri-list", 0, TARGET_URILIST},
    { "UTF8_STRING", 0, TARGET_UTF8_STRING },
    { "STRING", 0, 0 },
    { "TEXT",   0, 0 }, 
    { "COMPOUND_TEXT", 0, 0 }
  };
  static gint n_drag_types = sizeof(drag_types)/sizeof(drag_types[0]);

  gtk_drag_dest_set (GTK_WIDGET (filesel),
		     GTK_DEST_DEFAULT_ALL,
		     drop_types, n_drop_types,
		     GDK_ACTION_COPY);

  gtk_signal_connect (GTK_OBJECT(filesel), "drag_data_received",
		      GTK_SIGNAL_FUNC(filenames_dropped), NULL);

  eventbox = gtk_widget_get_parent (filesel->selection_text);
  gtk_drag_source_set (eventbox,
		       GDK_BUTTON1_MASK,
		       drag_types, n_drag_types,
		       GDK_ACTION_COPY);

  gtk_signal_connect (GTK_OBJECT (eventbox),
		      "drag_data_get",
		      GTK_SIGNAL_FUNC (filenames_drag_get),
		      filesel);
}

GtkWidget*
gpa_file_selection_new (const gchar *title)
{
  GpaFileSelection *filesel;

  filesel = gtk_type_new (GPA_TYPE_FILE_SELECTION);
  gtk_window_set_title (GTK_WINDOW (filesel), title);
  gtk_dialog_set_has_separator (GTK_DIALOG (filesel), FALSE);

  file_selection_setup_dnd (filesel);
  
  return GTK_WIDGET (filesel);
}

void
gpa_file_selection_show_fileop_buttons (GpaFileSelection *filesel)
{
  g_return_if_fail (GPA_IS_FILE_SELECTION (filesel));
    
  /* delete, create directory, and rename */
  if (!filesel->fileop_c_dir) 
    {
      filesel->fileop_c_dir = gtk_button_new_with_mnemonic (_("Create Dir"));
      gtk_signal_connect (GTK_OBJECT (filesel->fileop_c_dir), "clicked",
			  (GtkSignalFunc) gpa_file_selection_create_dir, 
			  (gpointer) filesel);
      gtk_box_pack_start (GTK_BOX (filesel->button_area), 
			  filesel->fileop_c_dir, TRUE, TRUE, 0);
      gtk_widget_show (filesel->fileop_c_dir);
    }
	
  if (!filesel->fileop_del_file) 
    {
      filesel->fileop_del_file = gtk_button_new_with_mnemonic (_("Delete File"));
      gtk_signal_connect (GTK_OBJECT (filesel->fileop_del_file), "clicked",
			  (GtkSignalFunc) gpa_file_selection_delete_file, 
			  (gpointer) filesel);
      gtk_box_pack_start (GTK_BOX (filesel->button_area), 
			  filesel->fileop_del_file, TRUE, TRUE, 0);
      gtk_widget_show (filesel->fileop_del_file);
    }

  if (!filesel->fileop_ren_file)
    {
      filesel->fileop_ren_file = gtk_button_new_with_mnemonic (_("Rename File"));
      gtk_signal_connect (GTK_OBJECT (filesel->fileop_ren_file), "clicked",
			  (GtkSignalFunc) gpa_file_selection_rename_file, 
			  (gpointer) filesel);
      gtk_box_pack_start (GTK_BOX (filesel->button_area), 
			  filesel->fileop_ren_file, TRUE, TRUE, 0);
      gtk_widget_show (filesel->fileop_ren_file);
    }
  g_object_notify (G_OBJECT (filesel), "show_fileops");
  gtk_widget_queue_resize (GTK_WIDGET (filesel));
}

void       
gpa_file_selection_hide_fileop_buttons (GpaFileSelection *filesel)
{
  g_return_if_fail (GPA_IS_FILE_SELECTION (filesel));
    
  if (filesel->fileop_ren_file)
    {
      gtk_widget_destroy (filesel->fileop_ren_file);
      filesel->fileop_ren_file = NULL;
    }

  if (filesel->fileop_del_file)
    {
      gtk_widget_destroy (filesel->fileop_del_file);
      filesel->fileop_del_file = NULL;
    }

  if (filesel->fileop_c_dir)
    {
      gtk_widget_destroy (filesel->fileop_c_dir);
      filesel->fileop_c_dir = NULL;
    }
  g_object_notify (G_OBJECT (filesel), "show_fileops");
}



void
gpa_file_selection_set_filename (GpaFileSelection *filesel,
				 const gchar      *filename)
{
  gchar *buf;
  const char *name, *last_slash;

  g_return_if_fail (GPA_IS_FILE_SELECTION (filesel));
  g_return_if_fail (filename != NULL);

  last_slash = strrchr (filename, G_DIR_SEPARATOR);

  if (!last_slash)
    {
      buf = g_strdup ("");
      name = filename;
    }
  else
    {
      buf = g_strdup (filename);
      buf[last_slash - filename + 1] = 0;
      name = last_slash + 1;
    }

  gpa_file_selection_populate (filesel, buf, FALSE);

  if (filesel->selection_entry)
    gtk_entry_set_text (GTK_ENTRY (filesel->selection_entry), name);
  g_free (buf);
  g_object_notify (G_OBJECT (filesel), "filename");
}

/**
 * gpa_file_selection_get_filename:
 * @filesel: a #GpaFileSelection
 * 
 * This function returns the selected filename in the C runtime's
 * multibyte string encoding, which may or may not be the same as that
 * used by GTK+ (UTF-8). To convert to UTF-8, call g_filename_to_utf8().
 * The returned string points to a statically allocated buffer and
 * should be copied if you plan to keep it around.
 * 
 * Return value: currently-selected filename in locale's encoding
 **/
G_CONST_RETURN gchar*
gpa_file_selection_get_filename (GpaFileSelection *filesel)
{
  static gchar nothing[2] = "";
  static gchar something[MAXPATHLEN*2];
  char *sys_filename;
  const char *text;

  g_return_val_if_fail (GPA_IS_FILE_SELECTION (filesel), nothing);

#ifdef G_WITH_CYGWIN
  translate_win32_path (filesel);
#endif
  text = gtk_entry_get_text (GTK_ENTRY (filesel->selection_entry));
  if (text)
    {
      sys_filename = g_filename_from_utf8 (cmpl_completion_fullname (text, filesel->cmpl_state), -1, NULL, NULL, NULL);
      if (!sys_filename)
	return nothing;
      strncpy (something, sys_filename, sizeof (something));
      g_free (sys_filename);
      return something;
    }

  return nothing;
}

void
gpa_file_selection_complete (GpaFileSelection *filesel,
			     const gchar      *pattern)
{
  g_return_if_fail (GPA_IS_FILE_SELECTION (filesel));
  g_return_if_fail (pattern != NULL);

  if (filesel->selection_entry)
    gtk_entry_set_text (GTK_ENTRY (filesel->selection_entry), pattern);
  gpa_file_selection_populate (filesel, (gchar*) pattern, TRUE);
}

static void
gpa_file_selection_destroy (GtkObject *object)
{
  GpaFileSelection *filesel;
  GList *list;
  HistoryCallbackArg *callback_arg;
  
  g_return_if_fail (GPA_IS_FILE_SELECTION (object));
  
  filesel = GPA_FILE_SELECTION (object);
  
  if (filesel->fileop_dialog)
    {
      gtk_widget_destroy (filesel->fileop_dialog);
      filesel->fileop_dialog = NULL;
    }
  
  if (filesel->history_list)
    {
      list = filesel->history_list;
      while (list)
	{
	  callback_arg = list->data;
	  g_free (callback_arg->directory);
	  g_free (callback_arg);
	  list = list->next;
	}
      g_list_free (filesel->history_list);
      filesel->history_list = NULL;
    }

  if (filesel->cmpl_state)
    {
      cmpl_free_state (filesel->cmpl_state);
      filesel->cmpl_state = NULL;
    }
  
  GTK_OBJECT_CLASS (parent_class)->destroy (object);
}

static void
gpa_file_selection_finalize (GObject *object)
{
  GpaFileSelection *filesel = GPA_FILE_SELECTION (object);

  g_free (filesel->fileop_file);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* Begin file operations callbacks */

static void
gpa_file_selection_fileop_error (GpaFileSelection *fs,
				 gchar            *error_message)
{
  GtkWidget *dialog;
    
  g_return_if_fail (error_message != NULL);

  /* main dialog */
  dialog = gtk_message_dialog_new (GTK_WINDOW (fs),
				   GTK_DIALOG_DESTROY_WITH_PARENT,
				   GTK_MESSAGE_ERROR,
				   GTK_BUTTONS_CLOSE,
				   "%s", error_message);

  /* yes, we free it */
  g_free (error_message);

  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  gtk_signal_connect_object (GTK_OBJECT (dialog), "response",
			     (GtkSignalFunc) gtk_widget_destroy, 
			     (gpointer) dialog);

  gtk_widget_show (dialog);
}

static void
gpa_file_selection_fileop_destroy (GtkWidget *widget,
				   gpointer   data)
{
  GpaFileSelection *fs = data;

  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));
  
  fs->fileop_dialog = NULL;
}


static void
gpa_file_selection_create_dir_confirmed (GtkWidget *widget,
					 gpointer   data)
{
  GpaFileSelection *fs = data;
  const gchar *dirname;
  gchar *path;
  gchar *full_path;
  gchar *sys_full_path;
  gchar *buf;
  GError *error = NULL;
  CompletionState *cmpl_state;
  
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  dirname = gtk_entry_get_text (GTK_ENTRY (fs->fileop_entry));
  cmpl_state = (CompletionState*) fs->cmpl_state;
  path = cmpl_reference_position (cmpl_state);
  
  full_path = g_strconcat (path, G_DIR_SEPARATOR_S, dirname, NULL);
  sys_full_path = g_filename_from_utf8 (full_path, -1, NULL, NULL, &error);
  if (error)
    {
      if (g_error_matches (error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE))
	buf = g_strdup_printf (_("The directory name \"%s\" contains symbols that are not allowed in filenames"), dirname);
      else
	buf = g_strdup_printf (_("Error creating directory \"%s\": %s\n%s"), dirname, error->message,
			       _("You probably used symbols not allowed in filenames."));
      gpa_file_selection_fileop_error (fs, buf);
      g_error_free (error);
      goto out;
    }

  if (mkdir (sys_full_path, 0755) < 0) 
    {
      buf = g_strdup_printf (_("Error creating directory \"%s\": %s\n"), dirname,
			     g_strerror (errno));
      gpa_file_selection_fileop_error (fs, buf);
    }

 out:
  g_free (full_path);
  g_free (sys_full_path);
  
  gtk_widget_destroy (fs->fileop_dialog);
  gpa_file_selection_populate (fs, "", FALSE);
}
  
static void
gpa_file_selection_create_dir (GtkWidget *widget,
			       gpointer   data)
{
  GpaFileSelection *fs = data;
  GtkWidget *label;
  GtkWidget *dialog;
  GtkWidget *vbox;
  GtkWidget *button;

  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  if (fs->fileop_dialog)
    return;
  
  /* main dialog */
  dialog = gtk_dialog_new ();
  fs->fileop_dialog = dialog;
  gtk_signal_connect (GTK_OBJECT (dialog), "destroy",
		      (GtkSignalFunc) gpa_file_selection_fileop_destroy, 
		      (gpointer) fs);
  gtk_window_set_title (GTK_WINDOW (dialog), _("Create Directory"));
  gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_MOUSE);

  /* If file dialog is grabbed, grab option dialog */
  /* When option dialog is closed, file dialog will be grabbed again */
  if (GTK_WINDOW (fs)->modal)
      gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  vbox = gtk_vbox_new (FALSE, 0);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 8);
  gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), vbox,
		     FALSE, FALSE, 0);
  gtk_widget_show( vbox);
  
  label = gtk_label_new_with_mnemonic (_("_Directory name:"));
  gtk_misc_set_alignment(GTK_MISC (label), 0.0, 0.0);
  gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 5);
  gtk_widget_show (label);

  /*  The directory entry widget  */
  fs->fileop_entry = gtk_entry_new ();
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), fs->fileop_entry);
  gtk_box_pack_start (GTK_BOX (vbox), fs->fileop_entry, 
		      TRUE, TRUE, 5);
  GTK_WIDGET_SET_FLAGS (fs->fileop_entry, GTK_CAN_DEFAULT);
  gtk_widget_show (fs->fileop_entry);
  
  /* buttons */
  button = gtk_dialog_add_button (dialog, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
  gtk_button_correct_label (GTK_BUTTON (button), _("_Cancel"));
  gtk_signal_connect_object (GTK_OBJECT (button), "clicked",
			     (GtkSignalFunc) gtk_widget_destroy, 
			     (gpointer) dialog);
  button = gtk_dialog_add_button (dialog, GTK_STOCK_OK, GTK_RESPONSE_OK);
  gtk_button_correct_label (GTK_BUTTON (button), _("_Create"));
  gtk_signal_connect (GTK_OBJECT (button), "clicked",
		      (GtkSignalFunc) gpa_file_selection_create_dir_confirmed, 
		      (gpointer) fs);

  gtk_widget_grab_focus (fs->fileop_entry);
  gtk_widget_show (dialog);
}

static void
gpa_file_selection_delete_file_confirmed (GtkWidget *widget,
					  gpointer   data)
{
  GpaFileSelection *fs = data;
  CompletionState *cmpl_state;
  gchar *path;
  gchar *full_path;
  gchar *sys_full_path;
  GError *error = NULL;
  gchar *buf;
  
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  cmpl_state = (CompletionState*) fs->cmpl_state;
  path = cmpl_reference_position (cmpl_state);
  
  full_path = g_strconcat (path, G_DIR_SEPARATOR_S, fs->fileop_file, NULL);
  sys_full_path = g_filename_from_utf8 (full_path, -1, NULL, NULL, &error);
  if (error)
    {
      if (g_error_matches (error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE))
	buf = g_strdup_printf (_("The filename \"%s\" contains symbols that are not allowed in filenames"),
			       fs->fileop_file);
      else
	buf = g_strdup_printf (_("Error deleting file \"%s\": %s\n%s"),
			       fs->fileop_file, error->message,
			       _("It probably contains symbols not allowed in filenames."));
      
      gpa_file_selection_fileop_error (fs, buf);
      g_error_free (error);
      goto out;
    }

  if (unlink (sys_full_path) < 0) 
    {
      buf = g_strdup_printf (_("Error deleting file \"%s\": %s"),
			     fs->fileop_file, g_strerror (errno));
      gpa_file_selection_fileop_error (fs, buf);
    }
  
 out:
  g_free (full_path);
  g_free (sys_full_path);
  
  gtk_widget_destroy (fs->fileop_dialog);
  gpa_file_selection_populate (fs, "", FALSE);
}

static void
gpa_file_selection_delete_file (GtkWidget *widget,
				gpointer   data)
{
  GpaFileSelection *fs = data;
  GtkWidget *label;
  GtkWidget *vbox;
  GtkWidget *button;
  GtkWidget *dialog;
  const gchar *filename;
  gchar *buf;
  
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  if (fs->fileop_dialog)
	  return;

#ifdef G_WITH_CYGWIN
  translate_win32_path (fs);
#endif

  filename = gtk_entry_get_text (GTK_ENTRY (fs->selection_entry));
  if (strlen (filename) < 1)
    return;

  g_free (fs->fileop_file);
  fs->fileop_file = g_strdup (filename);
  
  /* main dialog */
  fs->fileop_dialog = dialog = gtk_dialog_new ();
  gtk_signal_connect (GTK_OBJECT (dialog), "destroy",
		      (GtkSignalFunc) gpa_file_selection_fileop_destroy, 
		      (gpointer) fs);
  gtk_window_set_title (GTK_WINDOW (dialog), _("Delete File"));
  gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_MOUSE);

  /* If file dialog is grabbed, grab option dialog */
  /* When option dialog is closed, file dialog will be grabbed again */
  if (GTK_WINDOW (fs)->modal)
      gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
  
  vbox = gtk_vbox_new (FALSE, 0);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 8);
  gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), vbox,
		     FALSE, FALSE, 0);
  gtk_widget_show (vbox);

  buf = g_strdup_printf (_("Really delete file \"%s\"?"), filename);
  label = gtk_label_new (buf);
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.0);
  gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 5);
  gtk_widget_show (label);
  g_free (buf);
  
  /* buttons */
  button = gtk_dialog_add_button (dialog, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
  gtk_button_correct_label (GTK_BUTTON (button), _("_Cancel"));
  gtk_signal_connect_object (GTK_OBJECT (button), "clicked",
			     (GtkSignalFunc) gtk_widget_destroy, 
			     (gpointer) dialog);
  button = gtk_dialog_add_button (dialog, GTK_STOCK_OK, GTK_RESPONSE_OK);
  gtk_button_correct_label (GTK_BUTTON (button), _("_Delete"));
  gtk_signal_connect (GTK_OBJECT (button), "clicked",
		      (GtkSignalFunc) gpa_file_selection_delete_file_confirmed, 
		      (gpointer) fs);
  
  gtk_widget_show (dialog);

}

static void
gpa_file_selection_rename_file_confirmed (GtkWidget *widget,
					  gpointer   data)
{
  GpaFileSelection *fs = data;
  gchar *buf;
  const gchar *file;
  gchar *path;
  gchar *new_filename;
  gchar *old_filename;
  gchar *sys_new_filename;
  gchar *sys_old_filename;
  CompletionState *cmpl_state;
  GError *error = NULL;
  
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  file = gtk_entry_get_text (GTK_ENTRY (fs->fileop_entry));
  cmpl_state = (CompletionState*) fs->cmpl_state;
  path = cmpl_reference_position (cmpl_state);
  
  new_filename = g_strconcat (path, G_DIR_SEPARATOR_S, file, NULL);
  old_filename = g_strconcat (path, G_DIR_SEPARATOR_S, fs->fileop_file, NULL);

  sys_new_filename = g_filename_from_utf8 (new_filename, -1, NULL, NULL, &error);
  if (error)
    {
      if (g_error_matches (error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE))
	buf = g_strdup_printf (_("The file name \"%s\" contains symbols that are not allowed in filenames"), new_filename);
      else
	buf = g_strdup_printf (_("Error renaming file to \"%s\": %s\n%s"),
			       new_filename, error->message,
			       _("You probably used symbols not allowed in filenames."));
      gpa_file_selection_fileop_error (fs, buf);
      g_error_free (error);
      goto out1;
    }

  sys_old_filename = g_filename_from_utf8 (old_filename, -1, NULL, NULL, &error);
  if (error)
    {
      if (g_error_matches (error, G_CONVERT_ERROR, G_CONVERT_ERROR_ILLEGAL_SEQUENCE))
	buf = g_strdup_printf (_("The file name \"%s\" contains symbols that are not allowed in filenames"), old_filename);
      else
	buf = g_strdup_printf (_("Error renaming file \"%s\": %s\n%s"),
			       old_filename, error->message,
			       _("It probably contains symbols not allowed in filenames."));
      gpa_file_selection_fileop_error (fs, buf);
      g_error_free (error);
      goto out2;
    }
  
  if (rename (sys_old_filename, sys_new_filename) < 0) 
    {
      buf = g_strdup_printf (_("Error renaming file \"%s\" to \"%s\": %s"),
			     sys_old_filename, sys_new_filename,
			     g_strerror (errno));
      gpa_file_selection_fileop_error (fs, buf);
    }
  
 out2:
  g_free (sys_old_filename);

 out1:
  g_free (new_filename);
  g_free (old_filename);
  g_free (sys_new_filename);
  
  gtk_widget_destroy (fs->fileop_dialog);
  gpa_file_selection_populate (fs, "", FALSE);
}
  
static void
gpa_file_selection_rename_file (GtkWidget *widget,
				gpointer   data)
{
  GpaFileSelection *fs = data;
  GtkWidget *label;
  GtkWidget *dialog;
  GtkWidget *vbox;
  GtkWidget *button;
  gchar *buf;
  
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  if (fs->fileop_dialog)
	  return;

  g_free (fs->fileop_file);
  fs->fileop_file = g_strdup (gtk_entry_get_text (GTK_ENTRY (fs->selection_entry)));
  if (strlen (fs->fileop_file) < 1)
    return;
  
  /* main dialog */
  fs->fileop_dialog = dialog = gtk_dialog_new ();
  gtk_signal_connect (GTK_OBJECT (dialog), "destroy",
		      (GtkSignalFunc) gpa_file_selection_fileop_destroy, 
		      (gpointer) fs);
  gtk_window_set_title (GTK_WINDOW (dialog), _("Rename File"));
  gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_MOUSE);

  /* If file dialog is grabbed, grab option dialog */
  /* When option dialog  closed, file dialog will be grabbed again */
  if (GTK_WINDOW (fs)->modal)
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
  
  vbox = gtk_vbox_new (FALSE, 0);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 8);
  gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), vbox,
		      FALSE, FALSE, 0);
  gtk_widget_show(vbox);
  
  buf = g_strdup_printf (_("Rename file \"%s\" to:"), fs->fileop_file);
  label = gtk_label_new(buf);
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.0);
  gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 5);
  gtk_widget_show (label);
  g_free (buf);

  /* New filename entry */
  fs->fileop_entry = gtk_entry_new ();
  gtk_box_pack_start (GTK_BOX (vbox), fs->fileop_entry, 
		      TRUE, TRUE, 5);
  GTK_WIDGET_SET_FLAGS (fs->fileop_entry, GTK_CAN_DEFAULT);
  gtk_widget_show (fs->fileop_entry);
  
  gtk_entry_set_text (GTK_ENTRY (fs->fileop_entry), fs->fileop_file);
  gtk_editable_select_region (GTK_EDITABLE (fs->fileop_entry),
			      0, strlen (fs->fileop_file));

  /* buttons */
  button = gtk_dialog_add_button (dialog, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
  gtk_button_correct_label (GTK_BUTTON (button), _("_Cancel"));
  gtk_signal_connect_object (GTK_OBJECT (button), "clicked",
			     (GtkSignalFunc) gtk_widget_destroy, 
			     (gpointer) dialog);
  button = gtk_dialog_add_button (dialog, GTK_STOCK_OK, GTK_RESPONSE_OK);
  gtk_button_correct_label (GTK_BUTTON (button), _("_Rename"));
  gtk_signal_connect (GTK_OBJECT (button), "clicked",
		      (GtkSignalFunc) gpa_file_selection_rename_file_confirmed, 
		      (gpointer) fs);

  gtk_widget_show (dialog);
}

static gint
gpa_file_selection_insert_text (GtkWidget   *widget,
				const gchar *new_text,
				gint         new_text_length,
				gint        *position,
				gpointer     user_data)
{
  gchar *filename;

  filename = g_filename_from_utf8 (new_text, new_text_length, NULL, NULL, NULL);

  if (!filename)
    {
      gdk_beep ();
      gtk_signal_emit_stop_by_name (GTK_OBJECT (widget), "insert_text");
      return FALSE;
    }
  
  g_free (filename);
  
  return TRUE;
}

static gint
gpa_file_selection_key_press (GtkWidget   *widget,
			      GdkEventKey *event,
			      gpointer     user_data)
{
  GpaFileSelection *fs;
  char *text;

  g_return_val_if_fail (widget != NULL, FALSE);
  g_return_val_if_fail (event != NULL, FALSE);

  if (event->keyval == GDK_Tab ||
      event->keyval == GDK_ISO_Left_Tab ||
      event->keyval == GDK_KP_Tab)
    {
      fs = GPA_FILE_SELECTION (user_data);
#ifdef G_WITH_CYGWIN
      translate_win32_path (fs);
#endif
      text = g_strdup (gtk_entry_get_text (GTK_ENTRY (fs->selection_entry)));

      gpa_file_selection_populate (fs, text, TRUE);

      g_free (text);

      return TRUE;
    }

  return FALSE;
}


static void
gpa_file_selection_history_callback (GtkWidget *widget,
				     gpointer   data)
{
  GpaFileSelection *fs = data;
  HistoryCallbackArg *callback_arg;
  GList *list;

  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  list = fs->history_list;
  
  while (list) {
    callback_arg = list->data;
    
    if (callback_arg->menu_item == widget)
      {
	gpa_file_selection_populate (fs, callback_arg->directory, FALSE);
	break;
      }
    
    list = list->next;
  }
}

static void 
gpa_file_selection_update_history_menu (GpaFileSelection *fs,
					gchar            *current_directory)
{
  HistoryCallbackArg *callback_arg;
  GtkWidget *menu_item;
  GList *list;
  gchar *current_dir;
  gint dir_len;
  gint i;
  
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));
  g_return_if_fail (current_directory != NULL);
  
  list = fs->history_list;

  if (fs->history_menu) 
    {
      while (list) {
	callback_arg = list->data;
	g_free (callback_arg->directory);
	g_free (callback_arg);
	list = list->next;
      }
      g_list_free (fs->history_list);
      fs->history_list = NULL;
      
      gtk_widget_destroy (fs->history_menu);
    }
  
  fs->history_menu = gtk_menu_new ();

  current_dir = g_strdup (current_directory);

  dir_len = strlen (current_dir);

  for (i = dir_len; i >= 0; i--)
    {
      /* the i == dir_len is to catch the full path for the first 
       * entry. */
      if ( (current_dir[i] == G_DIR_SEPARATOR) || (i == dir_len))
	{
	  /* another small hack to catch the full path */
	  if (i != dir_len) 
		  current_dir[i + 1] = '\0';
#ifdef G_WITH_CYGWIN
	  if (!strcmp (current_dir, "//"))
	    continue;
#endif
	  menu_item = gtk_menu_item_new_with_mnemonic (current_dir);
	  
	  callback_arg = g_new (HistoryCallbackArg, 1);
	  callback_arg->menu_item = menu_item;
	  
	  /* since the autocompletion gets confused if you don't 
	   * supply a trailing '/' on a dir entry, set the full
	   * (current) path to "" which just refreshes the filesel */
	  if (dir_len == i)
	    {
	      callback_arg->directory = g_strdup ("");
	    }
	  else
	    {
	      callback_arg->directory = g_strdup (current_dir);
	    }
	  
	  fs->history_list = g_list_append (fs->history_list, callback_arg);
	  
	  gtk_signal_connect (GTK_OBJECT (menu_item), "activate",
			      (GtkSignalFunc) gpa_file_selection_history_callback,
			      (gpointer) fs);
	  gtk_menu_shell_append (GTK_MENU_SHELL (fs->history_menu), menu_item);
	  gtk_widget_show (menu_item);
	}
    }

  gtk_option_menu_set_menu (GTK_OPTION_MENU (fs->history_pulldown), 
			    fs->history_menu);
  g_free (current_dir);
}

static void
gpa_file_selection_file_button (GtkWidget      *widget,
				gint            row, 
				gint            column, 
				GdkEventButton *bevent,
				gpointer        user_data)
{
  GpaFileSelection *fs = NULL;
  gchar *filename, *temp = NULL;
  
  g_return_if_fail (GTK_IS_CLIST (widget));

  fs = user_data;
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));
  
  gtk_clist_get_text (GTK_CLIST (fs->file_list), row, 0, &temp);
  filename = g_strdup (temp);

#ifdef G_WITH_CYGWIN
  /* Check to see if the selection was a drive selector */
  if (isalpha (filename[0]) && (filename[1] == ':'))
    {
      /* It is... map it to a CYGWIN32 drive */
      gchar *temp_filename = g_strdup_printf ("//%c/", tolower (filename[0]));
      g_free(filename);
      filename = temp_filename;
    }
#endif /* G_WITH_CYGWIN */

  if (filename)
    {
      if (bevent)
	switch (bevent->type)
	  {
	  case GDK_2BUTTON_PRESS:
	    gtk_button_clicked (GTK_BUTTON (fs->ok_button));
	    break;
	    
	  default:
	    gtk_entry_set_text (GTK_ENTRY (fs->selection_entry), filename);
	    break;
	  }
      else
	gtk_entry_set_text (GTK_ENTRY (fs->selection_entry), filename);

      g_free (filename);
    }
}

static void
gpa_file_selection_dir_button (GtkWidget      *widget,
			       gint            row, 
			       gint            column, 
			       GdkEventButton *bevent,
			       gpointer        user_data)
{
  GpaFileSelection *fs = NULL;
  gchar *filename, *plain_filename, *temp = NULL;
  GtkCTreeNode *node;

  g_return_if_fail (GTK_IS_CLIST (widget));

  fs = GPA_FILE_SELECTION (user_data);
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));

  if (GTK_CLIST (fs->dir_list)->focus_row >= 0)
    node = GTK_CTREE_NODE (g_list_nth (GTK_CLIST (fs->dir_list)->row_list,
                                       GTK_CLIST (fs->dir_list)->focus_row));
  else
    node = GTK_CTREE_NODE (GTK_CLIST (fs->dir_list)->row_list);

  gtk_ctree_get_node_info (GTK_CTREE (fs->dir_list), node, &temp,
                           NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  plain_filename = g_strdup (temp);
  fprintf (stderr, "dir_button: plain_filename = %s\n", plain_filename);
  filename = g_strconcat (plain_filename, G_DIR_SEPARATOR_S, NULL);
  node = GTK_CTREE_ROW (node)->parent;
  while (node)
    {
      gchar *temp_filename;
      gtk_ctree_get_node_info (GTK_CTREE (fs->dir_list), node, &temp,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL);
      temp_filename = filename;
      filename = g_strconcat (temp, G_DIR_SEPARATOR_S, temp_filename, NULL);
      g_free (temp_filename);
      node = GTK_CTREE_ROW (node)->parent;
    }
#ifndef HAVE_DRIVE_LETTERS
  if (filename)
    {
      gchar *temp_filename = filename;
      filename = g_strconcat (G_DIR_SEPARATOR_S, temp_filename, NULL);
      g_free (temp_filename);
    }
#endif
  fprintf (stderr, "dir_button: filename = %s\n", filename);

  if (filename)
    {
      gpa_file_selection_populate (fs, filename, FALSE);
      gtk_entry_set_text (GTK_ENTRY (fs->selection_entry), plain_filename);
      g_free (filename);
    }
  if (plain_filename)
    g_free (plain_filename);
}

static void
gpa_file_selection_populate (GpaFileSelection *fs,
			     gchar            *rel_path,
			     gint              try_complete)
{
  CompletionState *cmpl_state;
  PossibleCompletion* poss;
  gchar* filename;
  gchar* rem_path = rel_path;
  gchar* sel_text;
  gchar* text[2];
  gint did_recurse = FALSE;
  gint possible_count = 0;
  gint selection_index = -1;
  
  fprintf (stderr, "populate: %s\n", rel_path);
  g_return_if_fail (GPA_IS_FILE_SELECTION (fs));
  
  cmpl_state = (CompletionState*) fs->cmpl_state;
  poss = cmpl_completion_matches (rel_path, &rem_path, cmpl_state);

  if (!cmpl_state_okay (cmpl_state))
    {
      /* Something went wrong. */
      gpa_file_selection_abort (fs);
      return;
    }

  g_assert (cmpl_state->reference_dir);

  gtk_clist_freeze (GTK_CLIST (fs->file_list));
  gtk_clist_clear (GTK_CLIST (fs->file_list));

  while (poss)
    {
      if (cmpl_is_a_completion (poss))
        {
          possible_count += 1;

          filename = cmpl_this_completion (poss);

	  text[0] = filename;
	  
          if (cmpl_is_directory (poss))
            /* Mark the entry in the tree, somehow. */;
          else
	    gtk_clist_append (GTK_CLIST (fs->file_list), text);
	}
      poss = cmpl_next_completion (cmpl_state);
    }

  gtk_clist_thaw (GTK_CLIST (fs->file_list));

  /* File lists are set. */

  g_assert (cmpl_state->reference_dir);

  if (try_complete)
    {

      /* User is trying to complete filenames, so advance the user's input
       * string to the updated_text, which is the common leading substring
       * of all possible completions, and if its a directory attempt
       * attempt completions in it. */

      if (cmpl_updated_text (cmpl_state)[0])
        {

          if (cmpl_updated_dir (cmpl_state))
            {
	      gchar* dir_name = g_strdup (cmpl_updated_text (cmpl_state));

              did_recurse = TRUE;

              gpa_file_selection_populate (fs, dir_name, TRUE);

              g_free (dir_name);
            }
          else
            {
	      if (fs->selection_entry)
		      gtk_entry_set_text (GTK_ENTRY (fs->selection_entry),
					  cmpl_updated_text (cmpl_state));
            }
        }
      else
        {
          selection_index = cmpl_last_valid_char (cmpl_state) -
                            (strlen (rel_path) - strlen (rem_path));
	  if (fs->selection_entry)
	    gtk_entry_set_text (GTK_ENTRY (fs->selection_entry), rem_path);
        }
    }
  else
    {
      if (fs->selection_entry)
	gtk_entry_set_text (GTK_ENTRY (fs->selection_entry), "");
    }

  if (!did_recurse)
    {
      if (fs->selection_entry)
	gtk_entry_set_position (GTK_ENTRY (fs->selection_entry), selection_index);

      if (fs->selection_entry)
	{
	  sel_text = g_strconcat (_("Selection: "),
				  cmpl_reference_position (cmpl_state),
				  NULL);

	  gtk_label_set_text (GTK_LABEL (fs->selection_text), sel_text);
	  g_free (sel_text);
	}

      if (fs->history_pulldown) 
	{
	  gpa_file_selection_update_history_menu (fs, cmpl_reference_position (cmpl_state));
	}
      
    }
}

static void
gpa_file_selection_abort (GpaFileSelection *fs)
{
  gchar err_buf[256];

  sprintf (err_buf, _("Directory unreadable: %s"), cmpl_strerror (cmpl_errno));

  /*  BEEP gdk_beep();  */

  if (fs->selection_entry)
    gtk_label_set_text (GTK_LABEL (fs->selection_text), err_buf);
}

/**********************************************************************/
/*			  External Interface                          */
/**********************************************************************/

/* The four completion state selectors
 */
static gchar*
cmpl_updated_text (CompletionState *cmpl_state)
{
  return cmpl_state->updated_text;
}

static gboolean
cmpl_updated_dir (CompletionState *cmpl_state)
{
  return cmpl_state->re_complete;
}

static gchar*
cmpl_reference_position (CompletionState *cmpl_state)
{
  return cmpl_state->reference_dir->fullname;
}

static gint
cmpl_last_valid_char (CompletionState *cmpl_state)
{
  return cmpl_state->last_valid_char;
}

static gchar*
cmpl_completion_fullname (const gchar     *text,
			  CompletionState *cmpl_state)
{
  static char nothing[2] = "";

  if (!cmpl_state_okay (cmpl_state))
    {
      return nothing;
    }
  else if (g_path_is_absolute (text))
    {
      strcpy (cmpl_state->updated_text, text);
    }
#ifdef HAVE_PWD_H
  else if (text[0] == '~')
    {
      CompletionDir* dir;
      char* slash;

      dir = open_user_dir (text, cmpl_state);

      if (!dir)
	{
	  /* spencer says just return ~something, so
	   * for now just do it. */
	  strcpy (cmpl_state->updated_text, text);
	}
      else
	{

	  strcpy (cmpl_state->updated_text, dir->fullname);

	  slash = strchr (text, G_DIR_SEPARATOR);

	  if (slash)
	    strcat (cmpl_state->updated_text, slash);
	}
    }
#endif
  else
    {
      strcpy (cmpl_state->updated_text, cmpl_state->reference_dir->fullname);
      if (cmpl_state->updated_text[strlen (cmpl_state->updated_text) - 1] != G_DIR_SEPARATOR)
	strcat (cmpl_state->updated_text, G_DIR_SEPARATOR_S);
      strcat (cmpl_state->updated_text, text);
    }

  return cmpl_state->updated_text;
}

/* The three completion selectors
 */
static gchar*
cmpl_this_completion (PossibleCompletion* pc)
{
  return pc->text;
}

static gboolean
cmpl_is_directory (PossibleCompletion* pc)
{
  return pc->is_directory;
}

static gint
cmpl_is_a_completion (PossibleCompletion* pc)
{
  return pc->is_a_completion;
}

/**********************************************************************/
/*	                 Construction, deletion                       */
/**********************************************************************/

static CompletionState*
cmpl_init_state (void)
{
  gchar *sys_getcwd_buf;
  gchar *utf8_cwd;
  CompletionState *new_state;

  new_state = g_new (CompletionState, 1);

  /* g_get_current_dir() returns a string in the "system" charset */
  sys_getcwd_buf = g_get_current_dir ();
  utf8_cwd = g_filename_to_utf8 (sys_getcwd_buf, -1, NULL, NULL, NULL);
  g_free (sys_getcwd_buf);

tryagain:

  new_state->reference_dir = NULL;
  new_state->completion_dir = NULL;
  new_state->active_completion_dir = NULL;
  new_state->directory_storage = NULL;
  new_state->directory_sent_storage = NULL;
  new_state->last_valid_char = 0;
  new_state->updated_text = g_new (gchar, MAXPATHLEN);
  new_state->updated_text_alloc = MAXPATHLEN;
  new_state->the_completion.text = g_new (gchar, MAXPATHLEN);
  new_state->the_completion.text_alloc = MAXPATHLEN;
  new_state->user_dir_name_buffer = NULL;
  new_state->user_directories = NULL;

  new_state->reference_dir = open_dir (utf8_cwd, new_state);

  if (!new_state->reference_dir)
    {
      /* Directories changing from underneath us, grumble */
      strcpy (utf8_cwd, G_DIR_SEPARATOR_S);
      goto tryagain;
    }

  g_free (utf8_cwd);
  return new_state;
}

static void
cmpl_free_dir_list (GList* dp0)
{
  GList *dp = dp0;

  while (dp)
    {
      free_dir (dp->data);
      dp = dp->next;
    }

  g_list_free (dp0);
}

static void
cmpl_free_dir_sent_list (GList* dp0)
{
  GList *dp = dp0;

  while (dp)
    {
      free_dir_sent (dp->data);
      dp = dp->next;
    }

  g_list_free (dp0);
}

static void
cmpl_free_state (CompletionState* cmpl_state)
{
  g_return_if_fail (cmpl_state != NULL);

  cmpl_free_dir_list (cmpl_state->directory_storage);
  cmpl_free_dir_sent_list (cmpl_state->directory_sent_storage);

  if (cmpl_state->user_dir_name_buffer)
    g_free (cmpl_state->user_dir_name_buffer);
  if (cmpl_state->user_directories)
    g_free (cmpl_state->user_directories);
  if (cmpl_state->the_completion.text)
    g_free (cmpl_state->the_completion.text);
  if (cmpl_state->updated_text)
    g_free (cmpl_state->updated_text);

  g_free (cmpl_state);
}

static void
free_dir (CompletionDir* dir)
{
  g_free (dir->fullname);
  g_free (dir);
}

static void
free_dir_sent (CompletionDirSent* sent)
{
  gint i;
  for (i = 0; i < sent->entry_count; i++)
    g_free (sent->entries[i].entry_name);
  g_free (sent->entries);
  g_free (sent);
}

static void
prune_memory_usage (CompletionState *cmpl_state)
{
  GList* cdsl = cmpl_state->directory_sent_storage;
  GList* cdl = cmpl_state->directory_storage;
  GList* cdl0 = cdl;
  gint len = 0;

  for (; cdsl && len < CMPL_DIRECTORY_CACHE_SIZE; len += 1)
    cdsl = cdsl->next;

  if (cdsl)
    {
      cmpl_free_dir_sent_list (cdsl->next);
      cdsl->next = NULL;
    }

  cmpl_state->directory_storage = NULL;
  while (cdl)
    {
      if (cdl->data == cmpl_state->reference_dir)
	cmpl_state->directory_storage = g_list_prepend (NULL, cdl->data);
      else
	free_dir (cdl->data);
      cdl = cdl->next;
    }

  g_list_free (cdl0);
}

/**********************************************************************/
/*                        The main entrances.                         */
/**********************************************************************/

static PossibleCompletion*
cmpl_completion_matches (gchar           *text_to_complete,
			 gchar          **remaining_text,
			 CompletionState *cmpl_state)
{
#ifdef HAVE_PWD_H
  gchar* first_slash;
#endif
  PossibleCompletion *poss;

  prune_memory_usage (cmpl_state);

  g_assert (text_to_complete != NULL);

  cmpl_state->user_completion_index = -1;
  cmpl_state->last_completion_text = text_to_complete;
  cmpl_state->the_completion.text[0] = 0;
  cmpl_state->last_valid_char = 0;
  cmpl_state->updated_text_len = -1;
  cmpl_state->updated_text[0] = 0;
  cmpl_state->re_complete = FALSE;

#ifdef HAVE_PWD_H
  first_slash = strchr (text_to_complete, G_DIR_SEPARATOR);

  if (text_to_complete[0] == '~' && !first_slash)
    {
      /* Text starts with ~ and there is no slash, show all the
       * home directory completions.
       */
      poss = attempt_homedir_completion (text_to_complete, cmpl_state);

      update_cmpl (poss, cmpl_state);

      return poss;
    }
#endif
  cmpl_state->reference_dir =
    open_ref_dir (text_to_complete, remaining_text, cmpl_state);

  if (!cmpl_state->reference_dir)
    return NULL;

  cmpl_state->completion_dir =
    find_completion_dir (*remaining_text, remaining_text, cmpl_state);

  cmpl_state->last_valid_char = *remaining_text - text_to_complete;

  if (!cmpl_state->completion_dir)
    return NULL;

  cmpl_state->completion_dir->cmpl_index = -1;
  cmpl_state->completion_dir->cmpl_parent = NULL;
  cmpl_state->completion_dir->cmpl_text = *remaining_text;

  cmpl_state->active_completion_dir = cmpl_state->completion_dir;

  cmpl_state->reference_dir = cmpl_state->completion_dir;

  poss = attempt_file_completion (cmpl_state);

  update_cmpl (poss, cmpl_state);

  return poss;
}

static PossibleCompletion*
cmpl_next_completion (CompletionState* cmpl_state)
{
  PossibleCompletion* poss = NULL;

  cmpl_state->the_completion.text[0] = 0;

#ifdef HAVE_PWD_H
  if (cmpl_state->user_completion_index >= 0)
    poss = attempt_homedir_completion (cmpl_state->last_completion_text, cmpl_state);
  else
    poss = attempt_file_completion (cmpl_state);
#else
  poss = attempt_file_completion (cmpl_state);
#endif

  update_cmpl (poss, cmpl_state);

  return poss;
}

/**********************************************************************/
/*			 Directory Operations                         */
/**********************************************************************/

/* Open the directory where completion will begin from, if possible. */
static CompletionDir*
open_ref_dir (gchar           *text_to_complete,
	      gchar          **remaining_text,
	      CompletionState *cmpl_state)
{
  gchar* first_slash;
  CompletionDir *new_dir;

  first_slash = strchr (text_to_complete, G_DIR_SEPARATOR);

#ifdef G_WITH_CYGWIN
  if (text_to_complete[0] == '/' && text_to_complete[1] == '/')
    {
      char root_dir[5];
      sprintf (root_dir, "//%c", text_to_complete[2]);

      new_dir = open_dir (root_dir, cmpl_state);

      if (new_dir) {
	*remaining_text = text_to_complete + 4;
      }
    }
#else
  if (FALSE)
    ;
#endif
#ifdef HAVE_PWD_H
  else if (text_to_complete[0] == '~')
    {
      new_dir = open_user_dir (text_to_complete, cmpl_state);

      if (new_dir)
	{
	  if (first_slash)
	    *remaining_text = first_slash + 1;
	  else
	    *remaining_text = text_to_complete + strlen (text_to_complete);
	}
      else
	{
	  return NULL;
	}
    }
#endif
  else if (g_path_is_absolute (text_to_complete) || !cmpl_state->reference_dir)
    {
      gchar *tmp = g_strdup (text_to_complete);
      gchar *p;

      p = tmp;
      while (*p && *p != '*' && *p != '?')
	p++;

      *p = '\0';
      p = strrchr (tmp, G_DIR_SEPARATOR);
      if (p)
	{
	  if (p == tmp)
	    p++;
      
	  *p = '\0';

	  new_dir = open_dir (tmp, cmpl_state);

	  if (new_dir)
	    *remaining_text = text_to_complete + 
	      ((p == tmp + 1) ? (p - tmp) : (p + 1 - tmp));
	}
      else
	{
	  /* If no possible candidates, use the cwd */
	  gchar *sys_curdir = g_get_current_dir ();
	  gchar *utf8_curdir = g_filename_to_utf8 (sys_curdir, -1, NULL, NULL, NULL);

	  g_free (sys_curdir);

	  new_dir = open_dir (utf8_curdir, cmpl_state);

	  if (new_dir)
	    *remaining_text = text_to_complete;

	  g_free (utf8_curdir);
	}

      g_free (tmp);
    }
  else
    {
      *remaining_text = text_to_complete;

      new_dir = open_dir (cmpl_state->reference_dir->fullname, cmpl_state);
    }

  if (new_dir)
    {
      new_dir->cmpl_index = -1;
      new_dir->cmpl_parent = NULL;
    }

  return new_dir;
}

#ifdef HAVE_PWD_H

/* open a directory by user name */
static CompletionDir*
open_user_dir (const gchar     *text_to_complete,
	       CompletionState *cmpl_state)
{
  CompletionDir *result;
  gchar *first_slash;
  gint cmp_len;

  g_assert (text_to_complete && text_to_complete[0] == '~');

  first_slash = strchr (text_to_complete, G_DIR_SEPARATOR);

  if (first_slash)
    cmp_len = first_slash - text_to_complete - 1;
  else
    cmp_len = strlen (text_to_complete + 1);

  if (!cmp_len)
    {
      /* ~/ */
      const gchar *homedir = g_get_home_dir ();
      gchar *utf8_homedir = g_filename_to_utf8 (homedir, -1, NULL, NULL, NULL);

      if (utf8_homedir)
	result = open_dir (utf8_homedir, cmpl_state);
      else
	result = NULL;
      
      g_free (utf8_homedir);
    }
  else
    {
      /* ~user/ */
      gchar* copy = g_new (char, cmp_len + 1);
      gchar *utf8_dir;
      struct passwd *pwd;

      strncpy (copy, text_to_complete + 1, cmp_len);
      copy[cmp_len] = 0;
      pwd = getpwnam (copy);
      g_free (copy);
      if (!pwd)
	{
	  cmpl_errno = errno;
	  return NULL;
	}
      utf8_dir = g_filename_to_utf8 (pwd->pw_dir, -1, NULL, NULL, NULL);
      result = open_dir (utf8_dir, cmpl_state);
      g_free (utf8_dir);
    }
  return result;
}

#endif

/* open a directory relative the the current relative directory */
static CompletionDir*
open_relative_dir (gchar           *dir_name,
		   CompletionDir   *dir,
		   CompletionState *cmpl_state)
{
  CompletionDir *result;
  GString *path;

  path = g_string_sized_new (dir->fullname_len + strlen (dir_name) + 10);
  g_string_assign (path, dir->fullname);

  if (dir->fullname_len > 1
      && path->str[dir->fullname_len - 1] != G_DIR_SEPARATOR)
    g_string_append_c (path, G_DIR_SEPARATOR);
  g_string_append (path, dir_name);

  result = open_dir (path->str, cmpl_state);

  g_string_free (path, TRUE);

  return result;
}

/* after the cache lookup fails, really open a new directory */
static CompletionDirSent*
open_new_dir (gchar       *dir_name,
	      struct stat *sbuf,
	      gboolean     stat_subdirs)
{
  CompletionDirSent *sent;
  GDir *directory;
  const char *dirent;
  GError *error;
  gint entry_count = 0;
  gint n_entries = 0;
  gint i;
  struct stat ent_sbuf;
  GString *path;
  gchar *sys_dir_name;

  sent = g_new (CompletionDirSent, 1);
  sent->mtime = sbuf->st_mtime;
  sent->inode = sbuf->st_ino;
  sent->device = sbuf->st_dev;

  path = g_string_sized_new (2*MAXPATHLEN + 10);

  sys_dir_name = g_filename_from_utf8 (dir_name, -1, NULL, NULL, NULL);
  if (!sys_dir_name)
    {
      cmpl_errno = CMPL_ERRNO_DID_NOT_CONVERT;
      return NULL;
    }
  
  directory = g_dir_open (sys_dir_name, 0, &error);
  if (!directory)
    {
      cmpl_errno = error->code; /* ??? */
      g_free (sys_dir_name);
      return NULL;
    }

  while ((dirent = g_dir_read_name (directory)) != NULL)
    entry_count++;

  sent->entries = g_new (CompletionDirEntry, entry_count);
  sent->entry_count = entry_count;

  g_dir_rewind (directory);

  for (i = 0; i < entry_count; i += 1)
    {
      dirent = g_dir_read_name (directory);

      if (!dirent)
	{
	  g_warning ("Failure reading directory '%s'", sys_dir_name);
	  g_dir_close (directory);
	  g_free (sys_dir_name);
	  return NULL;
	}

      sent->entries[n_entries].entry_name = g_filename_to_utf8 (dirent, -1, NULL, NULL, NULL);
      if (!g_utf8_validate (sent->entries[n_entries].entry_name, -1, NULL))
	{
	  g_warning (_("The filename %s couldn't be converted to UTF-8. Try setting the environment variable G_BROKEN_FILENAMES."), dirent);
	  continue;
	}

      g_string_assign (path, sys_dir_name);
      if (path->str[path->len-1] != G_DIR_SEPARATOR)
	{
	  g_string_append_c (path, G_DIR_SEPARATOR);
	}
      g_string_append (path, dirent);

      if (stat_subdirs)
	{
	  /* Here we know path->str is a "system charset" string */
	  if (stat (path->str, &ent_sbuf) >= 0 && S_ISDIR (ent_sbuf.st_mode))
	    sent->entries[n_entries].is_dir = TRUE;
	  else
	    /* stat may fail, and we don't mind, since it could be a
	     * dangling symlink. */
	    sent->entries[n_entries].is_dir = FALSE;
	}
      else
	sent->entries[n_entries].is_dir = 1;

      n_entries++;
    }
  sent->entry_count = n_entries;
  
  g_free (sys_dir_name);
  g_string_free (path, TRUE);
  qsort (sent->entries, sent->entry_count, sizeof (CompletionDirEntry), compare_cmpl_dir);

  g_dir_close (directory);

  return sent;
}

#if !defined(G_OS_WIN32) && !defined(G_WITH_CYGWIN)

static gboolean
check_dir (gchar       *dir_name,
	   struct stat *result,
	   gboolean    *stat_subdirs)
{
  /* A list of directories that we know only contain other directories.
   * Trying to stat every file in these directories would be very
   * expensive.
   */

  static struct {
    gchar *name;
    gboolean present;
    struct stat statbuf;
  } no_stat_dirs[] = {
    { "/afs", FALSE, { 0 } },
    { "/net", FALSE, { 0 } }
  };

  static const gint n_no_stat_dirs = G_N_ELEMENTS (no_stat_dirs);
  static gboolean initialized = FALSE;
  gchar *sys_dir_name;
  gint i;

  if (!initialized)
    {
      initialized = TRUE;
      for (i = 0; i < n_no_stat_dirs; i++)
	{
	  if (stat (no_stat_dirs[i].name, &no_stat_dirs[i].statbuf) == 0)
	    no_stat_dirs[i].present = TRUE;
	}
    }

  sys_dir_name = g_filename_from_utf8 (dir_name, -1, NULL, NULL, NULL);
  if (!sys_dir_name)
    {
      cmpl_errno = CMPL_ERRNO_DID_NOT_CONVERT;
      return FALSE;
    }
  
  if (stat (sys_dir_name, result) < 0)
    {
      g_free (sys_dir_name);
      cmpl_errno = errno;
      return FALSE;
    }
  g_free (sys_dir_name);

  *stat_subdirs = TRUE;
  for (i = 0; i < n_no_stat_dirs; i++)
    {
      if (no_stat_dirs[i].present &&
	  (no_stat_dirs[i].statbuf.st_dev == result->st_dev) &&
	  (no_stat_dirs[i].statbuf.st_ino == result->st_ino))
	{
	  *stat_subdirs = FALSE;
	  break;
	}
    }

  return TRUE;
}

#endif

/* open a directory by absolute pathname */
static CompletionDir*
open_dir (gchar           *dir_name,
	  CompletionState *cmpl_state)
{
  struct stat sbuf;
  gboolean stat_subdirs;
  CompletionDirSent *sent;
  GList* cdsl;

#if !defined(G_OS_WIN32) && !defined(G_WITH_CYGWIN)
  if (!check_dir (dir_name, &sbuf, &stat_subdirs))
    return NULL;

  cdsl = cmpl_state->directory_sent_storage;

  while (cdsl)
    {
      sent = cdsl->data;

      if (sent->inode == sbuf.st_ino &&
	  sent->mtime == sbuf.st_mtime &&
	  sent->device == sbuf.st_dev)
	return attach_dir (sent, dir_name, cmpl_state);

      cdsl = cdsl->next;
    }
#else
  stat_subdirs = TRUE;
#endif

  sent = open_new_dir (dir_name, &sbuf, stat_subdirs);

  if (sent)
    {
      cmpl_state->directory_sent_storage =
	g_list_prepend (cmpl_state->directory_sent_storage, sent);

      return attach_dir (sent, dir_name, cmpl_state);
    }

  return NULL;
}

static CompletionDir*
attach_dir (CompletionDirSent *sent,
	    gchar             *dir_name,
	    CompletionState   *cmpl_state)
{
  CompletionDir* new_dir;

  new_dir = g_new (CompletionDir, 1);

  cmpl_state->directory_storage =
    g_list_prepend (cmpl_state->directory_storage, new_dir);

  new_dir->sent = sent;
  new_dir->fullname = g_strdup (dir_name);
  new_dir->fullname_len = strlen (dir_name);

  return new_dir;
}

static gint
correct_dir_fullname (CompletionDir* cmpl_dir)
{
  gint length = strlen (cmpl_dir->fullname);
  gchar *first_slash = strchr (cmpl_dir->fullname, G_DIR_SEPARATOR);
  gchar *sys_filename;
  struct stat sbuf;

  /* Does it end with /. (\.) ? */
  if (length >= 2 &&
      strcmp (cmpl_dir->fullname + length - 2, G_DIR_SEPARATOR_S ".") == 0)
    {
      /* Is it just the root directory (on a drive) ? */
      if (cmpl_dir->fullname + length - 2 == first_slash)
	{
	  cmpl_dir->fullname[length - 1] = 0;
	  cmpl_dir->fullname_len = length - 1;
	  return TRUE;
	}
      else
	{
	  cmpl_dir->fullname[length - 2] = 0;
	}
    }

  /* Ends with /./ (\.\)? */
  else if (length >= 3 &&
	   strcmp (cmpl_dir->fullname + length - 3,
		   G_DIR_SEPARATOR_S "." G_DIR_SEPARATOR_S) == 0)
    cmpl_dir->fullname[length - 2] = 0;

  /* Ends with /.. (\..) ? */
  else if (length >= 3 &&
	   strcmp (cmpl_dir->fullname + length - 3,
		   G_DIR_SEPARATOR_S "..") == 0)
    {
      /* Is it just /.. (X:\..)? */
      if (cmpl_dir->fullname + length - 3 == first_slash)
	{
	  cmpl_dir->fullname[length - 2] = 0;
	  cmpl_dir->fullname_len = length - 2;
	  return TRUE;
	}

      sys_filename = g_filename_from_utf8 (cmpl_dir->fullname, -1, NULL, NULL, NULL);
      if (!sys_filename)
	{
	  cmpl_errno = CMPL_ERRNO_DID_NOT_CONVERT;
	  return FALSE;
	}
      
      if (stat (sys_filename, &sbuf) < 0)
	{
	  g_free (sys_filename);
	  cmpl_errno = errno;
	  return FALSE;
	}
      g_free (sys_filename);

      cmpl_dir->fullname[length - 3] = 0;

      if (!correct_parent (cmpl_dir, &sbuf))
	return FALSE;
    }

  /* Ends with /../ (\..\)? */
  else if (length >= 4 &&
	   strcmp (cmpl_dir->fullname + length - 4,
		   G_DIR_SEPARATOR_S ".." G_DIR_SEPARATOR_S) == 0)
    {
      /* Is it just /../ (X:\..\)? */
      if (cmpl_dir->fullname + length - 4 == first_slash)
	{
	  cmpl_dir->fullname[length - 3] = 0;
	  cmpl_dir->fullname_len = length - 3;
	  return TRUE;
	}

      sys_filename = g_filename_from_utf8 (cmpl_dir->fullname, -1, NULL, NULL, NULL);
      if (!sys_filename)
	{
	  cmpl_errno = CMPL_ERRNO_DID_NOT_CONVERT;
	  return FALSE;
	}
      
      if (stat (sys_filename, &sbuf) < 0)
	{
	  g_free (sys_filename);
	  cmpl_errno = errno;
	  return FALSE;
	}
      g_free (sys_filename);

      cmpl_dir->fullname[length - 4] = 0;

      if (!correct_parent (cmpl_dir, &sbuf))
	return FALSE;
    }

  cmpl_dir->fullname_len = strlen (cmpl_dir->fullname);

  return TRUE;
}

static gint
correct_parent (CompletionDir *cmpl_dir,
		struct stat   *sbuf)
{
  struct stat parbuf;
  gchar *last_slash;
  gchar *first_slash;
  gchar *new_name;
  gchar *sys_filename;
  gchar c = 0;

  last_slash = strrchr (cmpl_dir->fullname, G_DIR_SEPARATOR);
  g_assert (last_slash);
  first_slash = strchr (cmpl_dir->fullname, G_DIR_SEPARATOR);

  /* Clever (?) way to check for top-level directory that works also on
   * Win32, where there is a drive letter and colon prefixed...
   */
  if (last_slash != first_slash)
    {
      last_slash[0] = 0;
    }
  else
    {
      c = last_slash[1];
      last_slash[1] = 0;
    }

  sys_filename = g_filename_from_utf8 (cmpl_dir->fullname, -1, NULL, NULL, NULL);
  if (!sys_filename)
    {
      cmpl_errno = CMPL_ERRNO_DID_NOT_CONVERT;
      if (!c)
	last_slash[0] = G_DIR_SEPARATOR;
      return FALSE;
    }
  
  if (stat (sys_filename, &parbuf) < 0)
    {
      g_free (sys_filename);
      cmpl_errno = errno;
      if (!c)
	last_slash[0] = G_DIR_SEPARATOR;
      return FALSE;
    }
  g_free (sys_filename);

#ifndef G_OS_WIN32		/* No inode numbers on Win32 */
  if (parbuf.st_ino == sbuf->st_ino && parbuf.st_dev == sbuf->st_dev)
    /* it wasn't a link */
    return TRUE;

  if (c)
    last_slash[1] = c;
  else
    last_slash[0] = G_DIR_SEPARATOR;

  /* it was a link, have to figure it out the hard way */

  new_name = find_parent_dir_fullname (cmpl_dir->fullname);

  if (!new_name)
    return FALSE;

  g_free (cmpl_dir->fullname);

  cmpl_dir->fullname = new_name;
#endif

  return TRUE;
}

#ifndef G_OS_WIN32

static gchar*
find_parent_dir_fullname (gchar* dirname)
{
  gchar *sys_orig_dir;
  gchar *result;
  gchar *sys_cwd;
  gchar *sys_dirname;

  sys_orig_dir = g_get_current_dir ();
  sys_dirname = g_filename_from_utf8 (dirname, -1, NULL, NULL, NULL);
  if (!sys_dirname)
    {
      g_free (sys_orig_dir);
      cmpl_errno = CMPL_ERRNO_DID_NOT_CONVERT;
      return NULL;
    }
  
  if (chdir (sys_dirname) != 0 || chdir ("..") != 0)
    {
      g_free (sys_dirname);
      g_free (sys_orig_dir);
      cmpl_errno = errno;
      return NULL;
    }
  g_free (sys_dirname);

  sys_cwd = g_get_current_dir ();
  result = g_filename_to_utf8 (sys_cwd, -1, NULL, NULL, NULL);
  g_free (sys_cwd);

  if (chdir (sys_orig_dir) != 0)
    {
      cmpl_errno = errno;
      g_free (sys_orig_dir);
      return NULL;
    }

  g_free (sys_orig_dir);
  return result;
}

#endif

/**********************************************************************/
/*                        Completion Operations                       */
/**********************************************************************/

#ifdef HAVE_PWD_H

static PossibleCompletion*
attempt_homedir_completion (gchar           *text_to_complete,
			    CompletionState *cmpl_state)
{
  gint index, length;

  if (!cmpl_state->user_dir_name_buffer &&
      !get_pwdb (cmpl_state))
    return NULL;
  length = strlen (text_to_complete) - 1;

  cmpl_state->user_completion_index += 1;

  while (cmpl_state->user_completion_index < cmpl_state->user_directories_len)
    {
      index = first_diff_index (text_to_complete + 1,
				cmpl_state->user_directories
				[cmpl_state->user_completion_index].login);

      switch (index)
	{
	case PATTERN_MATCH:
	  break;
	default:
	  if (cmpl_state->last_valid_char < (index + 1))
	    cmpl_state->last_valid_char = index + 1;
	  cmpl_state->user_completion_index += 1;
	  continue;
	}

      cmpl_state->the_completion.is_a_completion = 1;
      cmpl_state->the_completion.is_directory = TRUE;

      append_completion_text ("~", cmpl_state);

      append_completion_text (cmpl_state->
			      user_directories[cmpl_state->user_completion_index].login,
			      cmpl_state);

      return append_completion_text (G_DIR_SEPARATOR_S, cmpl_state);
    }

  if (text_to_complete[1]
      || cmpl_state->user_completion_index > cmpl_state->user_directories_len)
    {
      cmpl_state->user_completion_index = -1;
      return NULL;
    }
  else
    {
      cmpl_state->user_completion_index += 1;
      cmpl_state->the_completion.is_a_completion = 1;
      cmpl_state->the_completion.is_directory = TRUE;

      return append_completion_text ("~" G_DIR_SEPARATOR_S, cmpl_state);
    }
}

#endif

#if defined(G_OS_WIN32) || defined(G_WITH_CYGWIN)
#define FOLD(c) (tolower(c))
#else
#define FOLD(c) (c)
#endif

/* returns the index (>= 0) of the first differing character,
 * PATTERN_MATCH if the completion matches */
static gint
first_diff_index (gchar *pat,
		  gchar *text)
{
  gint diff = 0;

  while (*pat && *text && FOLD (*text) == FOLD (*pat))
    {
      pat += 1;
      text += 1;
      diff += 1;
    }

  if (*pat)
    return diff;

  return PATTERN_MATCH;
}

static PossibleCompletion*
append_completion_text (gchar           *text,
			CompletionState *cmpl_state)
{
  gint len, i = 1;

  if (!cmpl_state->the_completion.text)
    return NULL;

  len = strlen (text) + strlen (cmpl_state->the_completion.text) + 1;

  if (cmpl_state->the_completion.text_alloc > len)
    {
      strcat (cmpl_state->the_completion.text, text);
      return &cmpl_state->the_completion;
    }

  while (i < len)
    i <<= 1;

  cmpl_state->the_completion.text_alloc = i;

  cmpl_state->the_completion.text = (gchar*) g_realloc (cmpl_state->the_completion.text, i);

  if (!cmpl_state->the_completion.text)
    return NULL;
  else
    {
      strcat (cmpl_state->the_completion.text, text);
      return &cmpl_state->the_completion;
    }
}

static CompletionDir*
find_completion_dir (gchar          *text_to_complete,
		    gchar          **remaining_text,
		    CompletionState *cmpl_state)
{
  gchar* first_slash = strchr (text_to_complete, G_DIR_SEPARATOR);
  CompletionDir* dir = cmpl_state->reference_dir;
  CompletionDir* next;
  *remaining_text = text_to_complete;

  while (first_slash)
    {
      gint len = first_slash - *remaining_text;
      gint found = 0;
      gchar *found_name = NULL;         /* Quiet gcc */
      gint i;
      gchar* pat_buf = g_new (gchar, len + 1);

      strncpy (pat_buf, *remaining_text, len);
      pat_buf[len] = 0;

      for (i = 0; i < dir->sent->entry_count; i += 1)
	{
	  if (dir->sent->entries[i].is_dir &&
	     fnmatch (pat_buf, dir->sent->entries[i].entry_name,
		      FNMATCH_FLAGS)!= FNM_NOMATCH)
	    {
	      if (found)
		{
		  g_free (pat_buf);
		  return dir;
		}
	      else
		{
		  found = 1;
		  found_name = dir->sent->entries[i].entry_name;
		}
	    }
	}

      if (!found)
	{
	  /* Perhaps we are trying to open an automount directory */
	  found_name = pat_buf;
	}

      next = open_relative_dir (found_name, dir, cmpl_state);
      
      if (!next)
	{
	  g_free (pat_buf);
	  return NULL;
	}
      
      next->cmpl_parent = dir;
      
      dir = next;
      
      if (!correct_dir_fullname (dir))
	{
	  g_free (pat_buf);
	  return NULL;
	}
      
      *remaining_text = first_slash + 1;
      first_slash = strchr (*remaining_text, G_DIR_SEPARATOR);

      g_free (pat_buf);
    }

  return dir;
}

static void
update_cmpl (PossibleCompletion *poss,
	     CompletionState    *cmpl_state)
{
  gint cmpl_len;

  if (!poss || !cmpl_is_a_completion (poss))
    return;

  cmpl_len = strlen (cmpl_this_completion (poss));

  if (cmpl_state->updated_text_alloc < cmpl_len + 1)
    {
      cmpl_state->updated_text =
	(gchar*)g_realloc (cmpl_state->updated_text,
			   cmpl_state->updated_text_alloc);
      cmpl_state->updated_text_alloc = 2*cmpl_len;
    }

  if (cmpl_state->updated_text_len < 0)
    {
      strcpy (cmpl_state->updated_text, cmpl_this_completion (poss));
      cmpl_state->updated_text_len = cmpl_len;
      cmpl_state->re_complete = cmpl_is_directory (poss);
    }
  else if (cmpl_state->updated_text_len == 0)
    {
      cmpl_state->re_complete = FALSE;
    }
  else
    {
      gint first_diff =
	first_diff_index (cmpl_state->updated_text,
			  cmpl_this_completion (poss));

      cmpl_state->re_complete = FALSE;

      if (first_diff == PATTERN_MATCH)
	return;

      if (first_diff > cmpl_state->updated_text_len)
	strcpy (cmpl_state->updated_text, cmpl_this_completion (poss));

      cmpl_state->updated_text_len = first_diff;
      cmpl_state->updated_text[first_diff] = 0;
    }
}

static PossibleCompletion*
attempt_file_completion (CompletionState *cmpl_state)
{
  gchar *pat_buf, *first_slash;
  CompletionDir *dir = cmpl_state->active_completion_dir;

  dir->cmpl_index += 1;

  if (dir->cmpl_index == dir->sent->entry_count)
    {
      if (dir->cmpl_parent == NULL)
	{
	  cmpl_state->active_completion_dir = NULL;

	  return NULL;
	}
      else
	{
	  cmpl_state->active_completion_dir = dir->cmpl_parent;

	  return attempt_file_completion (cmpl_state);
	}
    }

  g_assert (dir->cmpl_text);

  first_slash = strchr (dir->cmpl_text, G_DIR_SEPARATOR);

  if (first_slash)
    {
      gint len = first_slash - dir->cmpl_text;

      pat_buf = g_new (gchar, len + 1);
      strncpy (pat_buf, dir->cmpl_text, len);
      pat_buf[len] = 0;
    }
  else
    {
      gint len = strlen (dir->cmpl_text);

      pat_buf = g_new (gchar, len + 2);
      strcpy (pat_buf, dir->cmpl_text);
      /* Don't append a * if the user entered one herself.
       * This way one can complete *.h and don't get matches
       * on any .help files, for instance.
       */
      if (strchr (pat_buf, '*') == NULL)
	strcpy (pat_buf + len, "*");
    }

  if (first_slash)
    {
      if (dir->sent->entries[dir->cmpl_index].is_dir)
	{
	  if (fnmatch (pat_buf, dir->sent->entries[dir->cmpl_index].entry_name,
		       FNMATCH_FLAGS) != FNM_NOMATCH)
	    {
	      CompletionDir* new_dir;

	      new_dir = open_relative_dir (dir->sent->entries[dir->cmpl_index].entry_name,
					   dir, cmpl_state);

	      if (!new_dir)
		{
		  g_free (pat_buf);
		  return NULL;
		}

	      new_dir->cmpl_parent = dir;

	      new_dir->cmpl_index = -1;
	      new_dir->cmpl_text = first_slash + 1;

	      cmpl_state->active_completion_dir = new_dir;

	      g_free (pat_buf);
	      return attempt_file_completion (cmpl_state);
	    }
	  else
	    {
	      g_free (pat_buf);
	      return attempt_file_completion (cmpl_state);
	    }
	}
      else
	{
	  g_free (pat_buf);
	  return attempt_file_completion (cmpl_state);
	}
    }
  else
    {
      if (dir->cmpl_parent != NULL)
	{
	  append_completion_text (dir->fullname +
				  strlen (cmpl_state->completion_dir->fullname) + 1,
				  cmpl_state);
	  append_completion_text (G_DIR_SEPARATOR_S, cmpl_state);
	}

      append_completion_text (dir->sent->entries[dir->cmpl_index].entry_name, cmpl_state);

      cmpl_state->the_completion.is_a_completion =
	fnmatch (pat_buf, dir->sent->entries[dir->cmpl_index].entry_name,
		 FNMATCH_FLAGS) != FNM_NOMATCH;

      cmpl_state->the_completion.is_directory = dir->sent->entries[dir->cmpl_index].is_dir;
      if (dir->sent->entries[dir->cmpl_index].is_dir)
	append_completion_text (G_DIR_SEPARATOR_S, cmpl_state);

      g_free (pat_buf);
      return &cmpl_state->the_completion;
    }
}

#ifdef HAVE_PWD_H

static gint
get_pwdb (CompletionState* cmpl_state)
{
  struct passwd *pwd_ptr;
  gchar* buf_ptr;
  gchar *utf8;
  gint len = 0, i, count = 0;

  if (cmpl_state->user_dir_name_buffer)
    return TRUE;
  setpwent ();

  while ((pwd_ptr = getpwent ()) != NULL)
    {
      utf8 = g_filename_to_utf8 (pwd_ptr->pw_name, -1, NULL, NULL, NULL);
      len += strlen (utf8);
      g_free (utf8);
      utf8 = g_filename_to_utf8 (pwd_ptr->pw_dir, -1, NULL, NULL, NULL);
      len += strlen (utf8);
      g_free (utf8);
      len += 2;
      count += 1;
    }

  setpwent ();

  cmpl_state->user_dir_name_buffer = g_new (gchar, len);
  cmpl_state->user_directories = g_new (CompletionUserDir, count);
  cmpl_state->user_directories_len = count;

  buf_ptr = cmpl_state->user_dir_name_buffer;

  for (i = 0; i < count; i += 1)
    {
      pwd_ptr = getpwent ();
      if (!pwd_ptr)
	{
	  cmpl_errno = errno;
	  goto error;
	}

      utf8 = g_filename_to_utf8 (pwd_ptr->pw_name, -1, NULL, NULL, NULL);
      strcpy (buf_ptr, utf8);
      g_free (utf8);

      cmpl_state->user_directories[i].login = buf_ptr;

      buf_ptr += strlen (buf_ptr);
      buf_ptr += 1;

      utf8 = g_filename_to_utf8 (pwd_ptr->pw_dir, -1, NULL, NULL, NULL);
      strcpy (buf_ptr, utf8);
      g_free (utf8);

      cmpl_state->user_directories[i].homedir = buf_ptr;

      buf_ptr += strlen (buf_ptr);
      buf_ptr += 1;
    }

  qsort (cmpl_state->user_directories,
	 cmpl_state->user_directories_len,
	 sizeof (CompletionUserDir),
	 compare_user_dir);

  endpwent ();

  return TRUE;

error:

  if (cmpl_state->user_dir_name_buffer)
    g_free (cmpl_state->user_dir_name_buffer);
  if (cmpl_state->user_directories)
    g_free (cmpl_state->user_directories);

  cmpl_state->user_dir_name_buffer = NULL;
  cmpl_state->user_directories = NULL;

  return FALSE;
}

static gint
compare_user_dir (const void *a,
		  const void *b)
{
  return strcmp ((((CompletionUserDir*)a))->login,
		 (((CompletionUserDir*)b))->login);
}

#endif

static gint
compare_cmpl_dir (const void *a,
		  const void *b)
{
#if !defined(G_OS_WIN32) && !defined(G_WITH_CYGWIN)
  return strcmp ((((CompletionDirEntry*)a))->entry_name,
		 (((CompletionDirEntry*)b))->entry_name);
#else
  return g_strcasecmp ((((CompletionDirEntry*)a))->entry_name,
		       (((CompletionDirEntry*)b))->entry_name);
#endif
}

static gint
cmpl_state_okay (CompletionState* cmpl_state)
{
  return  cmpl_state && cmpl_state->reference_dir;
}

static const gchar*
cmpl_strerror (gint err)
{
  if (err == CMPL_ERRNO_TOO_LONG)
    return _("Name too long");
  else if (err == CMPL_ERRNO_DID_NOT_CONVERT)
    return _("Couldn't convert filename");
  else
    return g_strerror (err);
}
