/* keyeditdlg.c  -	 The GNU Privacy Assistant
 *	Copyright (C) 2000, 2001 G-N-U GmbH.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <gpapa.h>
#include <gtk/gtk.h>
#include "gpa.h"
#include "gpapastrings.h"
#include "gtktools.h"
#include "siglist.h"
#include "ownertrustdlg.h"
#include "keyeditdlg.h"

typedef struct {
  GtkWidget * window;
  GtkWidget * expiry;
  GtkWidget * ownertrust;
  gchar * key_id;
  gboolean key_has_changed;
  gboolean destroy_window;
} GPAKeyEditDialog;


/* internal API */
static gboolean key_edit_delete_event (GtkWidget *widget, GdkEvent *event,
				       gpointer param);
static void key_edit_close (GtkWidget *widget, gpointer param);

static GtkWidget * add_details_row (GtkWidget * table, gint row, gchar *label,
				    gchar * text, gboolean selectable);

static void key_edit_change_trust(GtkWidget * widget, gpointer param);


/* run the key edit dialog as a modal dialog */
gboolean
gpa_key_edit_dialog_run (GtkWidget * parent, gchar * key_id)
{
  GtkWidget * window;
  GtkWidget * vbox;
  GtkWidget * frame;
  GtkWidget * hbox;
  GtkWidget * label;
  GtkWidget * button;
  GtkWidget * table;
  GtkWidget * bbox;
  GtkWidget * vbox_sign;
  GtkWidget * siglist;
  GtkWidget * scrolled;

  GpapaPublicKey * public_key;
  GpapaSecretKey * secret_key;
  GpapaOwnertrust trust;
  GList * signatures;
  GDate * expiry_date;
  gchar * date_string;

  GPAKeyEditDialog dialog;

  dialog.key_id = key_id;
  dialog.key_has_changed = FALSE;

  public_key = gpapa_get_public_key_by_ID (key_id, gpa_callback, parent);
  secret_key = gpapa_get_secret_key_by_ID (key_id, gpa_callback, parent);
  
  window = gtk_window_new (GTK_WINDOW_DIALOG);
  dialog.window = window;
  gtk_window_set_title (GTK_WINDOW (window), _("Edit Key"));
  gtk_container_set_border_width (GTK_CONTAINER (window), 5);
  gtk_signal_connect (GTK_OBJECT (window), "delete-event",
		      GTK_SIGNAL_FUNC (key_edit_delete_event),
		      &dialog);

  vbox = gtk_vbox_new (FALSE, 5);
  gtk_container_add (GTK_CONTAINER (window), vbox);

  /* info about the key */
  table = gtk_table_new (2, 3, FALSE);
  gtk_box_pack_start (GTK_BOX (vbox), table, FALSE, TRUE, 0);
  gtk_table_set_row_spacing (GTK_TABLE (table), 0, 2);
  gtk_table_set_col_spacing (GTK_TABLE (table), 0, 4);
  
  add_details_row (table, 0, _("User Name:"),
		   gpapa_key_get_name (GPAPA_KEY (public_key), gpa_callback,
				       parent), FALSE);
  add_details_row (table, 1, _("Key ID:"),
		   gpapa_key_get_identifier (GPAPA_KEY (public_key),
					     gpa_callback, parent),
		   FALSE);


  /* change expiry date */
  frame = gtk_frame_new (_("Expiry Date"));
  gtk_box_pack_start (GTK_BOX (vbox), frame, FALSE, TRUE, 5);
  
  hbox = gtk_hbox_new (FALSE, 0);
  gtk_container_add (GTK_CONTAINER (frame), hbox);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 5);

  expiry_date = gpapa_key_get_expiry_date (GPAPA_KEY (public_key),
					   gpa_callback, parent);
  date_string = gpa_expiry_date_string (expiry_date);
  label = gtk_label_new (date_string);
  free (date_string);
  dialog.expiry = label;
  gtk_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 0);

  button = gtk_button_new_with_label (_("Change"));
  gtk_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);
  gtk_widget_set_sensitive (button, secret_key != NULL);


  /* Owner Trust */
  frame = gtk_frame_new (_("Owner Trust"));
  gtk_box_pack_start (GTK_BOX (vbox), frame, FALSE, TRUE, 5);
  
  hbox = gtk_hbox_new (FALSE, 0);
  gtk_container_add (GTK_CONTAINER (frame), hbox);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 5);

  trust = gpapa_public_key_get_ownertrust (public_key, gpa_callback, parent);
  label = gtk_label_new (gpa_ownertrust_string (trust));
  dialog.ownertrust = label;
  gtk_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 0);

  button = gtk_button_new_with_label (_("Change"));
  gtk_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);
  gtk_signal_connect (GTK_OBJECT (button), "clicked",
		      (GtkSignalFunc)key_edit_change_trust, &dialog);


  /* Sign */
  frame = gtk_frame_new (_("Signatures"));
  gtk_box_pack_start (GTK_BOX (vbox), frame, TRUE, TRUE, 5);

  vbox_sign = gtk_vbox_new (FALSE, 0);
  gtk_container_add (GTK_CONTAINER (frame), vbox_sign);
  gtk_container_set_border_width (GTK_CONTAINER (vbox_sign), 5);

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_box_pack_start (GTK_BOX (vbox_sign), scrolled, TRUE, TRUE, 0);

  siglist = gpa_siglist_new (window);
  gtk_container_add (GTK_CONTAINER (scrolled), siglist);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
				  GTK_POLICY_AUTOMATIC,
				  GTK_POLICY_AUTOMATIC);
  signatures = gpapa_public_key_get_signatures (public_key, gpa_callback,
						parent);
  gpa_siglist_set_signatures (siglist, signatures, key_id);

  bbox = gtk_hbutton_box_new ();
  gtk_box_pack_start (GTK_BOX (vbox_sign), bbox, FALSE, TRUE, 5);
  gtk_button_box_set_layout (GTK_BUTTON_BOX (bbox), GTK_BUTTONBOX_END);

  button = gtk_button_new_with_label (_("Sign"));
  gtk_box_pack_start (GTK_BOX (bbox), button, FALSE, FALSE, 0);


  /* buttons */
  bbox = gtk_hbutton_box_new ();
  gtk_box_pack_start (GTK_BOX (vbox), bbox, FALSE, TRUE, 5);
  gtk_button_box_set_layout (GTK_BUTTON_BOX (bbox), GTK_BUTTONBOX_END);

  button = gtk_button_new_with_label (_("Close"));
  gtk_box_pack_start (GTK_BOX (bbox), button, FALSE, TRUE, 0);
  gtk_signal_connect (GTK_OBJECT (button), "clicked",
		      (GtkSignalFunc)key_edit_close, NULL);

  gtk_window_set_modal (GTK_WINDOW (window), TRUE);
  gpa_window_show_centered (window, parent);
  dialog.destroy_window = TRUE;

  gtk_main ();

  if (dialog.destroy_window)
    gtk_widget_destroy (window);

  return dialog.key_has_changed;
}

/* add a single row to the details table */
static GtkWidget *
add_details_row (GtkWidget * table, gint row, gchar *label_text,
		 gchar * text, gboolean selectable)
{
  GtkWidget * widget;

  widget = gtk_label_new (label_text);
  gtk_table_attach (GTK_TABLE (table), widget, 0, 1, row, row + 1,
		    GTK_FILL, 0, 0, 0);
  gtk_misc_set_alignment (GTK_MISC (widget), 1.0, 0.5);

  if (!selectable)
    {
      widget = gtk_label_new (text);
      gtk_misc_set_alignment (GTK_MISC (widget), 0.0, 0.5);
    }
  else
    {
      widget = gtk_entry_new ();
      gtk_editable_set_editable (GTK_EDITABLE (widget), FALSE);
      gtk_entry_set_text (GTK_ENTRY (widget), text);
    }
  gtk_table_attach (GTK_TABLE (table), widget, 1, 2, row, row + 1,
		    GTK_FILL | GTK_EXPAND, 0, 0, 0);

  return widget;
}


/* signal handler for the delete-event siganl */
static gboolean
key_edit_delete_event (GtkWidget *widget, GdkEvent *event, gpointer param)
{
  GPAKeyEditDialog * dialog = param;

  gtk_main_quit ();
  dialog->destroy_window = FALSE;

  return FALSE;
}

/* signal handler for the close button */
static void
key_edit_close (GtkWidget *widget, gpointer param)
{
  gtk_main_quit ();
}


/* signal handler for the owner trust change button. */
static void
key_edit_change_trust(GtkWidget * widget, gpointer param)
{
  GPAKeyEditDialog * dialog = param;
  GpapaPublicKey * key;
  GpapaOwnertrust ownertrust;
  gboolean result;

  key = gpapa_get_public_key_by_ID (dialog->key_id, gpa_callback,
				    dialog->window);
  result = gpa_ownertrust_run_dialog (key, dialog->window,
				      "keyring_editor_public_edit_trust.tip",
				      &ownertrust);

  if (result)
    {
      gpapa_public_key_set_ownertrust (key, ownertrust, gpa_callback,
				       dialog->window);

      /*
      ownertrust = gpapa_public_key_get_ownertrust (key, gpa_callback,
						    dialog->window);
						    */
      gtk_label_set_text (GTK_LABEL (dialog->ownertrust),
		      gpa_ownertrust_string (ownertrust));
      dialog->key_has_changed = TRUE;
    }
}
  