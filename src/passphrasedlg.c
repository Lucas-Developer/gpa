/* passphrasedlg.c  -  The GNU Privacy Assistant
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include "gpa.h"
#include "gtktools.h"
#include "passphrasedlg.h"

/* a pointer to a GPAPassphraseDialog struct is passed to all callbacks
 * used in passphrase dialog and is used to pass widget pointers into
 * the signal handlers so that they can read their contents and to pass
 * information from the callbacks back to the gpa_passphrase_run_dialog
 * function.
 */
struct _GPAPassphraseDialog {
  /* the entry widget for the passphrase */
  GtkWidget * entry;
  
  /* The OK button handler sets passphrase to a copy of the contents of
   * the entry widget, the Cancel handler sets it to NULL. This field is
   * also used to indicate whether the user cancelled the dialog or not.
   */
  gchar * passphrase;
};
typedef struct _GPAPassphraseDialog GPAPassphraseDialog;


/* Signal handler for the OK button. Copy the entered password to the
 * dialog struct and quit the recursive mainloop
 */
static void
passphrase_ok (gpointer param)
{
  GPAPassphraseDialog * dialog = param;

  dialog->passphrase = xstrdup (gtk_entry_get_text (GTK_ENTRY(dialog->entry)));
  gtk_main_quit ();
}

/* Signal handler for the Cancel button. Set the passpghrase in the
 * dialog struct to NULL and quit the recursive mainloop
 */
static void
passphrase_cancel (gpointer param)
{
  GPAPassphraseDialog * dialog = param;

  dialog->passphrase = NULL;
  gtk_main_quit ();
}

/* signal handler for the dialog winow's delete-event. Just call the
 * cancel signal handler and return FALSE to indicate that the window may
 * be closed.
 */
static gboolean
passphrase_delete_event (GtkWidget *widget, GdkEvent *event, gpointer param)
{
  passphrase_cancel (param);
  return FALSE;
}

/* Create a modal dialog to ask for the passphrase, run the main-loop
 * recursively and return the entered passphrase if the user clicked OK,
 * and NULL if the user cancelled. The returned string uses malloced
 * memory and has to be free()ed by the caller.
 */
gchar * 
gpa_passphrase_run_dialog (GtkWidget * parent)
{
  GtkAccelGroup *accelGroup;
  GtkWidget *windowPassphrase;
  GtkWidget *vboxPassphrase;
  GtkWidget *hboxPasswd;
  GtkWidget *labelPasswd;
  GtkWidget *entryPasswd;
  GtkWidget *hButtonBoxPassphrase;
  GtkWidget *buttonCancel;
  GtkWidget *buttonOK;
  GPAPassphraseDialog dialog;

  dialog.passphrase = NULL;

  windowPassphrase = gtk_window_new (GTK_WINDOW_DIALOG);
  gtk_window_set_title (GTK_WINDOW (windowPassphrase), _("Enter Password"));
  gtk_signal_connect (GTK_OBJECT (windowPassphrase), "delete_event",
		      GTK_SIGNAL_FUNC (passphrase_delete_event),
		      (gpointer)&dialog);

  accelGroup = gtk_accel_group_new ();
  gtk_window_add_accel_group (GTK_WINDOW (windowPassphrase), accelGroup);

  vboxPassphrase = gtk_vbox_new (FALSE, 0);
  gtk_container_set_border_width (GTK_CONTAINER (vboxPassphrase), 5);

  hboxPasswd = gtk_hbox_new (FALSE, 0);
  gtk_container_set_border_width (GTK_CONTAINER (hboxPasswd), 5);

  labelPasswd = gtk_label_new ("");
  gtk_box_pack_start (GTK_BOX (hboxPasswd), labelPasswd, FALSE, FALSE, 0);
  entryPasswd = dialog.entry = gtk_entry_new ();
  gtk_entry_set_visibility (GTK_ENTRY (entryPasswd), FALSE);
  gtk_signal_connect_object (GTK_OBJECT (entryPasswd), "activate",
			     GTK_SIGNAL_FUNC (passphrase_ok),
			     (gpointer) &dialog);
  gtk_box_pack_start (GTK_BOX (hboxPasswd), entryPasswd, TRUE, TRUE, 0);
  gpa_connect_by_accelerator (GTK_LABEL (labelPasswd), entryPasswd,
			      accelGroup, _("_Password: "));
  gtk_box_pack_start (GTK_BOX (vboxPassphrase), hboxPasswd, TRUE, TRUE, 0);

  hButtonBoxPassphrase = gtk_hbutton_box_new ();
  gtk_button_box_set_layout (GTK_BUTTON_BOX (hButtonBoxPassphrase),
			     GTK_BUTTONBOX_END);
  gtk_button_box_set_spacing (GTK_BUTTON_BOX (hButtonBoxPassphrase), 10);
  buttonCancel = gpa_button_cancel_new (accelGroup, _("_Cancel"),
					passphrase_cancel, (gpointer) &dialog);
  gtk_container_set_border_width (GTK_CONTAINER (hButtonBoxPassphrase), 5);
  gtk_container_add (GTK_CONTAINER (hButtonBoxPassphrase), buttonCancel);
  buttonOK = gpa_button_new (accelGroup, _("_OK"));
  gtk_signal_connect_object (GTK_OBJECT (buttonOK), "clicked",
			     GTK_SIGNAL_FUNC (passphrase_ok),
			     (gpointer) &dialog);
  gtk_container_add (GTK_CONTAINER (hButtonBoxPassphrase), buttonOK);
  gtk_box_pack_start (GTK_BOX (vboxPassphrase), hButtonBoxPassphrase, FALSE,
		      FALSE, 0);
  gtk_container_add (GTK_CONTAINER (windowPassphrase), vboxPassphrase);
  gpa_window_show_centered (windowPassphrase, parent);

  gtk_grab_add (windowPassphrase);
  gtk_main ();
  gtk_grab_remove (windowPassphrase);
  gtk_widget_destroy (windowPassphrase);

  return dialog.passphrase;
} /* gpa_passphrase_run_dialog */