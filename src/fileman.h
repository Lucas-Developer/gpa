/* fileman.h  -  The GNU Privacy Assistant
 *	Copyright (C) 2000 G-N-U GmbH.
 *
 * This file is part of GPA
 *
 * GPA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GPA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#ifndef FILEMAN_H
#define FILEMAN_H

#include <gtk/gtk.h>

/* GObject stuff */
#define GPA_FILE_MANAGER_TYPE	  (gpa_file_manager_get_type ())
#define GPA_FILE_MANAGER(obj)	  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GPA_FILE_MANAGER_TYPE, GpaFileManager))
#define GPA_FILE_MANAGER_CLASS(klass)  (G_TYPE_CHECK_CLASS_CAST ((klass), GPA_FILE_MANAGER_TYPE, GpaFileManagerClass))
#define GPA_IS_FILE_MANAGER(obj)	  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GPA_FILE_MANAGER_TYPE))
#define GPA_IS_FILE_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GPA_FILE_MANAGER_TYPE))
#define GPA_FILE_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GPA_FILE_MANAGER_TYPE, GpaFileManagerClass))

typedef struct _GpaFileManager GpaFileManager;
typedef struct _GpaFileManagerClass GpaFileManagerClass;

struct _GpaFileManager {
  GtkWindow parent;

  GtkWidget *window;
  GtkWidget *list_files;
  GList *selection_sensitive_widgets;
};

struct _GpaFileManagerClass {
  GtkWindowClass parent_class;
};

GType gpa_file_manager_get_type (void) G_GNUC_CONST;

/* API */

GtkWidget * gpa_file_manager_get_instance (void);

gboolean gpa_file_manager_is_open (void);

void gpa_file_manager_open_file (GpaFileManager *fileman,
				 const gchar *filename);


#endif /* FILEMAN_H */
