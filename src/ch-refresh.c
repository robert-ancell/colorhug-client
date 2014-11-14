/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009-2014 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <colord.h>
#include <colord-gtk.h>
#include <math.h>
#include <gusb.h>
#include <colorhug.h>

#include "ch-cleanup.h"
#include "ch-graph-widget.h"

typedef struct {
	ChDeviceQueue		*device_queue;
	GSettings		*settings;
	GtkApplication		*application;
	GtkBuilder		*builder;
	GtkWidget		*graph;
	GtkWidget		*sample_widget;
	guint			 timer_id;
	GUsbContext		*usb_ctx;
	GUsbDevice		*device;
	GUsbDeviceList		*device_list;
	GTimer			*measured;
	gdouble			 usb_latency;		/* ms */
	gdouble			 time_per_idx;		/* ms */
} ChRefreshPrivate;

#define NR_DATA_POINTS	1365
#define NR_PULSES	5
#define NR_PULSE_GAP	400	/* ms */

/**
 * ch_refresh_error_dialog:
 **/
static void
ch_refresh_error_dialog (ChRefreshPrivate *priv,
			 const gchar *title,
			 const gchar *message)
{
	GtkWindow *window;
	GtkWidget *dialog;

	window = GTK_WINDOW(gtk_builder_get_object (priv->builder, "dialog_refresh"));
	dialog = gtk_message_dialog_new (window,
					 GTK_DIALOG_MODAL,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_CLOSE,
					 "%s", title);
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						  "%s", message);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}

/**
 * ch_refresh_activate_cb:
 **/
static void
ch_refresh_activate_cb (GApplication *application, ChRefreshPrivate *priv)
{
	GtkWindow *window;
	window = GTK_WINDOW (gtk_builder_get_object (priv->builder, "dialog_refresh"));
	gtk_window_present (window);
}

/**
 * ch_refresh_sample_set_black_cb:
 **/
static gboolean
ch_refresh_sample_set_black_cb (gpointer user_data)
{
	ChRefreshPrivate *priv = (ChRefreshPrivate *) user_data;
	CdColorRGB source;

	/* set to black */
	cd_color_rgb_set (&source, 0.f, 0.f, 0.f);
	cd_sample_widget_set_color (CD_SAMPLE_WIDGET (priv->sample_widget), &source);
	g_debug ("hiding patch at %fms",
		 g_timer_elapsed (priv->measured, NULL) * 1000.f);
	return FALSE;
}

/**
 * ch_refresh_sample_set_white_cb:
 **/
static gboolean
ch_refresh_sample_set_white_cb (gpointer user_data)
{
	ChRefreshPrivate *priv = (ChRefreshPrivate *) user_data;
	CdColorRGB source;

	/* set white */
	cd_color_rgb_set (&source, 1.f, 1.f, 1.f);
	cd_sample_widget_set_color (CD_SAMPLE_WIDGET (priv->sample_widget), &source);
	g_debug ("showing patch at %fms",
		 g_timer_elapsed (priv->measured, NULL) * 1000.f);

	/* set a timeout set the patch black again */
	g_timeout_add (100, ch_refresh_sample_set_black_cb, priv);
	return FALSE;
}

/**
 * ch_refresh_calc_average:
 **/
static gdouble
ch_refresh_calc_average (const gdouble *data, guint data_len)
{
	gdouble tmp = 0.f;
	guint i;
	for (i = 0; i < data_len; i++)
		tmp += data[i];
	return tmp / (gdouble) data_len;
}

/**
 * ch_refresh_calc_jitter:
 **/
static gdouble
ch_refresh_calc_jitter (const gdouble *data, guint data_len)
{
	gdouble ave;
	gdouble jitter = 0.f;
	gdouble tmp;
	guint i;
	ave = ch_refresh_calc_average (data, data_len);
	for (i = 0; i < data_len; i ++) {
		tmp = ABS (data[i] - ave);
		if (tmp > jitter)
			jitter = tmp;
	}
	return jitter;
}

/**
 * ch_refresh_get_usb_speed:
 **/
static gboolean
ch_refresh_get_usb_speed (ChRefreshPrivate *priv,
			  gdouble *latency,
			  gdouble *jitter,
			  GError **error)
{
	gboolean ret;
	gdouble elapsed[10];
	guint8 hw_version;
	guint i;
	_cleanup_timer_destroy_ GTimer *timer = NULL;

	timer = g_timer_new ();
	for (i = 0; i < 10; i ++) {
		g_timer_reset (timer);
		ch_device_queue_get_hardware_version (priv->device_queue,
						      priv->device,
						      &hw_version);
		ret = ch_device_queue_process (priv->device_queue,
					       CH_DEVICE_QUEUE_PROCESS_FLAGS_NONE,
					       NULL,
					       error);
		if (!ret)
			return FALSE;
		elapsed[i] = g_timer_elapsed (timer, NULL) * 1000;
	}

	/* calculate average and jitter */
	if (latency != NULL)
		*latency = ch_refresh_calc_average (elapsed, 10);
	if (jitter != NULL)
		*jitter = ch_refresh_calc_jitter (elapsed, 10);
	return TRUE;
}

/**
 * ch_refresh_update_ui:
 **/
static void
ch_refresh_update_ui (ChRefreshPrivate *priv)
{
	ChPointObj *point;
	const gchar *title;
	gboolean ret;
	gboolean zoom;
	gdouble data[NR_DATA_POINTS][3];
	gdouble pulse_data[NR_PULSES];
	gdouble tmp;
	GtkWidget *w;
	guint16 buffer[4096];
	guint cnt = 0;
	guint i;
	guint idx_start = 0;
	guint j;
	_cleanup_error_free_ GError *error = NULL;

	/* set the graph x scale */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "switch_zoom"));
	zoom = gtk_switch_get_active (GTK_SWITCH (w));
	tmp = zoom ? NR_DATA_POINTS * priv->time_per_idx / 5000 :
		     NR_DATA_POINTS * priv->time_per_idx / 1000;
	tmp = ceilf (tmp * 10.f) / 10.f;
	if (zoom) {
		g_object_set (priv->graph,
			      "start-x", tmp * 2,
			      "stop-x", tmp * 3,
			      NULL);
	} else {
		g_object_set (priv->graph,
			      "start-x", 0.f,
			      "stop-x", tmp,
			      NULL);
	}

	/* get 3694 samples from the sram */
	ch_device_queue_read_sram (priv->device_queue,
				   priv->device,
				   0x0000,
				   (guint8 *) buffer,
				   sizeof(buffer));
	ret = ch_device_queue_process (priv->device_queue,
				       CH_DEVICE_QUEUE_PROCESS_FLAGS_NONE,
				       NULL,
				       &error);
	if (!ret) {
		/* TRANSLATORS: permissions error perhaps? */
		title = _("Failed to get samples from device");
		ch_refresh_error_dialog (priv, title, error->message);
		return;
	}

	/* extract data */
	for (j = 0; j < 3; j++) {
		for (i = 0; i < NR_DATA_POINTS; i++)
			data[i][j] = buffer[i * 3 + j];
	}

	/* autoscale vertical */
	tmp = 0.f;
	for (j = 0; j < 3; j++) {
		for (i = 0; i < NR_DATA_POINTS; i++) {
			if (data[i][j] > tmp)
				tmp = data[i][j];
		}
	}
	for (j = 0; j < 3; j++) {
		for (i = 0; i < NR_DATA_POINTS; i++)
			data[i][j] /= tmp;
	}

	/* find rise time (10% -> 90% transition) */
	for (i = 0; i < NR_DATA_POINTS; i++) {
		if (data[i][1] < 0.1) {
			idx_start = 0;
			continue;
		}
		if (data[i][1] > 0.1 && data[i][1] < 0.9 && idx_start == 0) {
			idx_start = i;
			continue;
		}
		if (data[i][1] > 0.9 && idx_start > 0) {
			if (cnt < NR_PULSES)
				pulse_data[cnt] = (i - idx_start) * priv->time_per_idx;
			idx_start = 0;
			cnt++;
			continue;
		}
	}
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "label_rise"));
	if (cnt == NR_PULSES) {
		_cleanup_free_ gchar *str = NULL;
		str = g_strdup_printf ("%.0fms ±%.1fms",
				       ch_refresh_calc_average (pulse_data, NR_PULSES),
				       ch_refresh_calc_jitter (pulse_data, NR_PULSES));
		gtk_label_set_label (GTK_LABEL (w), str);
	} else {
		gtk_label_set_label (GTK_LABEL (w), "n/a (PWM?)");
	}

	/* find fall time (90% -> 10% transition) */
	cnt = 0;
	for (i = 0; i < NR_DATA_POINTS; i++) {
		if (data[i][1] > 0.9) {
			idx_start = i;
			continue;
		}
		if (data[i][1] < 0.1 && idx_start > 0) {
			if (cnt < NR_PULSES)
				pulse_data[cnt] = (i - idx_start) * priv->time_per_idx;
			idx_start = 0;
			cnt++;
			continue;
		}
	}
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "label_fall"));
	if (cnt == NR_PULSES) {
		_cleanup_free_ gchar *str = NULL;
		str = g_strdup_printf ("%.0fms ±%.1fms",
				       ch_refresh_calc_average (pulse_data, NR_PULSES),
				       ch_refresh_calc_jitter (pulse_data, NR_PULSES));
		gtk_label_set_label (GTK_LABEL (w), str);
	} else {
		gtk_label_set_label (GTK_LABEL (w), "n/a (PWM?)");
	}

	/* find display latency */
	cnt = 0;
	idx_start = 0;
	for (i = 0; i < NR_DATA_POINTS; i++) {
		if (data[i][1] < 0.1 && idx_start > 0) {
			idx_start = 0;
			continue;
		}
		if (data[i][1] > 0.1 && idx_start == 0) {
			idx_start = i;
			if (cnt < NR_PULSES) {
				pulse_data[cnt] = (gdouble) i * priv->time_per_idx;
				g_debug ("peak at %i = %f", i, pulse_data[cnt]);
			}
			cnt++;
			continue;
		}
	}
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "label_display_latency"));
	if (cnt == 5) {
		_cleanup_free_ gchar *str = NULL;
		for (i = 0; i < NR_PULSES; i++) {
			pulse_data[i] = pulse_data[i] - (NR_PULSE_GAP * i);
			g_debug ("peak %i difference is %f", i, pulse_data[i]);
		}
		str = g_strdup_printf ("%.0fms ±%.1fms",
				       ch_refresh_calc_average (pulse_data + 2, 3),
				       ch_refresh_calc_jitter (pulse_data + 2, 3));
		gtk_label_set_label (GTK_LABEL (w), str);
	} else {
		gtk_label_set_label (GTK_LABEL (w), "n/a");
	}

	/* render BW/RGB graphs */
	ch_graph_widget_clear (CH_GRAPH_WIDGET (priv->graph));
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "switch_channels"));
	if (gtk_switch_get_active (GTK_SWITCH (w))) {
		for (j = 0; j < 3; j++) {
			_cleanup_ptrarray_unref_ GPtrArray *array = NULL;
			array = g_ptr_array_new_with_free_func ((GDestroyNotify) ch_point_obj_free);
			for (i = 0; i < NR_DATA_POINTS; i++) {
				point = ch_point_obj_new ();
				point->x = ((gdouble) i) * priv->time_per_idx / 1000.f;
				point->y = data[i][j];
				point->color = 0x0000df << (j * 8);
				g_ptr_array_add (array, point);
			}
			ch_graph_widget_assign (CH_GRAPH_WIDGET (priv->graph),
						 CH_GRAPH_WIDGET_PLOT_LINE,
						 array);
		}
	} else {
		_cleanup_ptrarray_unref_ GPtrArray *array = NULL;
		array = g_ptr_array_new_with_free_func ((GDestroyNotify) ch_point_obj_free);
		for (i = 0; i < NR_DATA_POINTS; i++) {
			point = ch_point_obj_new ();
			point->x = ((gdouble) i) * priv->time_per_idx / 1000.f;
			point->y = ch_refresh_calc_average (data[i], 3);
			point->color = 0x000000;
			g_ptr_array_add (array, point);
		}
		ch_graph_widget_assign (CH_GRAPH_WIDGET (priv->graph),
					 CH_GRAPH_WIDGET_PLOT_LINE,
					 array);
	}

	/* add trigger lines */
	if (!zoom) {
		for (j = 0; j < NR_PULSES; j++) {
			_cleanup_ptrarray_unref_ GPtrArray *array = NULL;
			array = g_ptr_array_new_with_free_func ((GDestroyNotify) ch_point_obj_free);

			/* bottom */
			point = ch_point_obj_new ();
			point->x = ((gdouble) j) * (gdouble) NR_PULSE_GAP / 1000.f;
			point->y = 0.f;
			point->color = 0xfff000;
			g_ptr_array_add (array, point);

			/* top */
			point = ch_point_obj_new ();
			point->x = ((gdouble) j) * (gdouble) NR_PULSE_GAP / 1000.f;
			point->y = 1.0;
			point->color = 0xfff000;
			g_ptr_array_add (array, point);
			ch_graph_widget_assign (CH_GRAPH_WIDGET (priv->graph),
						 CH_GRAPH_WIDGET_PLOT_LINE,
						 array);
		}
	}
}

/**
 * ch_refresh_get_readings:
 **/
static void
ch_refresh_get_readings_cb (GObject *source, GAsyncResult *res, gpointer data)
{
	ChRefreshPrivate *priv = (ChRefreshPrivate *) data;
	const gchar *title;
	gdouble tmp;
	_cleanup_error_free_ GError *error = NULL;

	/* check success */
	if (!ch_device_queue_process_finish (priv->device_queue, res, &error)) {
		/* TRANSLATORS: permissions error perhaps? */
		title = _("Failed to get samples from device");
		ch_refresh_error_dialog (priv, title, error->message);
		return;
	}

	/* calculate how long each sample took */
	tmp = g_timer_elapsed (priv->measured, NULL) * 1000.f;
	g_debug ("taking sample took %.2fms", tmp);
	priv->time_per_idx = (tmp - priv->usb_latency) / (gdouble) NR_DATA_POINTS;
	g_debug ("each sample took %.2fms", priv->time_per_idx);
	ch_refresh_update_ui (priv);
}

/**
 * ch_refresh_get_readings:
 **/
static void
ch_refresh_get_readings (ChRefreshPrivate *priv)
{
	GtkWidget *w;
	const gchar *title;
	gdouble usb_jitter = 0.f;
	guint8 reading_array[30];
	guint i;
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_free_ gchar *usb_jitter_str = NULL;
	_cleanup_free_ gchar *usb_latency_str = NULL;

	/* measure new USB values */
	if (!ch_refresh_get_usb_speed (priv, &priv->usb_latency, &usb_jitter, &error)) {
		/* TRANSLATORS: permissions error perhaps? */
		title = _("Failed to calculate USB latency");
		ch_refresh_error_dialog (priv, title, error->message);
		return;
	}

	/* update USB labels */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "label_usb_latency"));
	usb_latency_str = g_strdup_printf ("%.1fms ±%.1fms", priv->usb_latency, usb_jitter);
	gtk_label_set_label (GTK_LABEL (w), usb_latency_str);

	/* do NR_PULSES white flashes, 350ms apart */
	g_idle_add (ch_refresh_sample_set_white_cb, priv);
	for (i = 1; i < NR_PULSES; i++)
		g_timeout_add (NR_PULSE_GAP * i, ch_refresh_sample_set_white_cb, priv);

	/* start taking a reading */
	g_timer_reset (priv->measured);
	ch_device_queue_take_reading_array (priv->device_queue,
					    priv->device,
					    reading_array);
	ch_device_queue_process_async (priv->device_queue,
				       CH_DEVICE_QUEUE_PROCESS_FLAGS_NONE,
				       NULL,
				       ch_refresh_get_readings_cb,
				       priv);
}

/**
 * ch_refresh_refresh_button_cb:
 **/
static void
ch_refresh_refresh_button_cb (GtkWidget *widget, ChRefreshPrivate *priv)
{
	/* get the latest from the device */
	ch_refresh_get_readings (priv);
}

/**
 * ch_refresh_zoom_changed_cb:
 **/
static void
ch_refresh_zoom_changed_cb (GObject *object, GParamSpec *pspec, ChRefreshPrivate *priv)
{
	ch_refresh_update_ui (priv);
}

/**
 * ch_refresh_device_close:
 **/
static void
ch_refresh_device_close (ChRefreshPrivate *priv)
{
	GtkWidget *w;
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "notebook_refresh"));
	gtk_notebook_set_current_page (GTK_NOTEBOOK (w), 1);
}

/**
 * ch_refresh_device_open:
 **/
static void
ch_refresh_device_open (ChRefreshPrivate *priv)
{
	GtkWidget *w;
	const gchar *title;
	_cleanup_error_free_ GError *error = NULL;

	/* open device */
	if (!ch_device_open (priv->device, &error)) {
		/* TRANSLATORS: permissions error perhaps? */
		title = _("Failed to open device");
		ch_refresh_error_dialog (priv, title, error->message);
		return;
	}
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "notebook_refresh"));
	gtk_notebook_set_current_page (GTK_NOTEBOOK (w), 0);
}

/**
 * ch_refresh_startup_cb:
 **/
static void
ch_refresh_startup_cb (GApplication *application, ChRefreshPrivate *priv)
{
	CdColorRGB source;
	GtkBox *box;
	GtkWidget *main_window;
	GtkWidget *w;
	gint retval;
	_cleanup_error_free_ GError *error = NULL;
	_cleanup_object_unref_ GdkPixbuf *pixbuf = NULL;

	/* get UI */
	priv->builder = gtk_builder_new ();
	retval = gtk_builder_add_from_resource (priv->builder,
						"/com/hughski/colorhug/ch-refresh.ui",
						&error);
	if (retval == 0) {
		g_warning ("failed to load ui: %s", error->message);
		return;
	}

	/* add application specific icons to search path */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   CH_DATA G_DIR_SEPARATOR_S "icons");

	main_window = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_refresh"));
	gtk_application_add_window (priv->application, GTK_WINDOW (main_window));
	gtk_widget_set_size_request (main_window, 400, 100);

	/* Hide window first so that the dialogue resizes itself without redrawing */
	gtk_widget_hide (main_window);

	/* buttons */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_refresh"));
	g_signal_connect (w, "clicked",
			  G_CALLBACK (ch_refresh_refresh_button_cb), priv);
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "switch_zoom"));
	g_signal_connect (w, "notify::active",
			  G_CALLBACK (ch_refresh_zoom_changed_cb), priv);
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "switch_channels"));
	g_signal_connect (w, "notify::active",
			  G_CALLBACK (ch_refresh_zoom_changed_cb), priv);

	/* set connect device page */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "notebook_refresh"));
	gtk_notebook_set_current_page (GTK_NOTEBOOK (w), 1);

	/* setup USB image */
	w = GTK_WIDGET (gtk_builder_get_object (priv->builder, "image_usb"));
	pixbuf = gdk_pixbuf_new_from_file_at_scale (CH_DATA
						    G_DIR_SEPARATOR_S "icons"
						    G_DIR_SEPARATOR_S "usb.svg",
						    -1, 48, TRUE, &error);
	g_assert (pixbuf != NULL);
	gtk_image_set_from_pixbuf (GTK_IMAGE (w), pixbuf);

	/* add graph */
	box = GTK_BOX (gtk_builder_get_object (priv->builder, "box_graph"));
	priv->graph = ch_graph_widget_new ();
	g_object_set (priv->graph,
		      "type-x", CH_GRAPH_WIDGET_TYPE_TIME,
		      "type-y", CH_GRAPH_WIDGET_TYPE_FACTOR,
		      "start-x", 0.f,
		      "stop-x", 2.f,
		      "start-y", 0.f,
		      "stop-y", 1.f,
		      "use-grid", TRUE,
		      NULL);
	gtk_box_pack_start (box, priv->graph, TRUE, TRUE, 0);
	gtk_widget_set_margin_top (priv->graph, 6);
	gtk_widget_set_margin_start (priv->graph, 12);
	gtk_widget_set_margin_end (priv->graph, 12);
	gtk_widget_set_size_request (priv->graph, 800, 450);
	gtk_widget_show (priv->graph);

	/* add sample widget */
	box = GTK_BOX (gtk_builder_get_object (priv->builder, "box_sidebar"));
	priv->sample_widget = cd_sample_widget_new ();
	gtk_box_pack_start (box, priv->sample_widget, TRUE, TRUE, 0);
	gtk_widget_show (priv->sample_widget);

	/* set black */
	source.R = 0.0f;
	source.G = 0.0f;
	source.B = 0.0f;
	cd_sample_widget_set_color (CD_SAMPLE_WIDGET (priv->sample_widget), &source);

	/* is the colorhug already plugged in? */
	g_usb_device_list_coldplug (priv->device_list);

	/* show main UI */
	gtk_widget_show (main_window);
}

/**
 * ch_refresh_device_added_cb:
 **/
static void
ch_refresh_device_added_cb (GUsbDeviceList *list,
			 GUsbDevice *device,
			 ChRefreshPrivate *priv)
{
	g_debug ("Added: %i:%i",
		 g_usb_device_get_vid (device),
		 g_usb_device_get_pid (device));
	if (ch_device_get_mode (device) == CH_DEVICE_MODE_FIRMWARE2) {
		priv->device = g_object_ref (device);
		ch_refresh_device_open (priv);
	}
}

/**
 * ch_refresh_device_removed_cb:
 **/
static void
ch_refresh_device_removed_cb (GUsbDeviceList *list,
			    GUsbDevice *device,
			    ChRefreshPrivate *priv)
{
	g_debug ("Removed: %i:%i",
		 g_usb_device_get_vid (device),
		 g_usb_device_get_pid (device));
	if (ch_device_get_mode (device) == CH_DEVICE_MODE_FIRMWARE2) {
		if (priv->device != NULL)
			g_object_unref (priv->device);
		priv->device = NULL;
		ch_refresh_device_close (priv);
	}
}

/**
 * main:
 **/
int
main (int argc, char **argv)
{
	ChRefreshPrivate *priv;
	gboolean verbose = FALSE;
	GOptionContext *context;
	int status = 0;
	_cleanup_error_free_ GError *error = NULL;
	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			/* TRANSLATORS: command line option */
			_("Show extra debugging information"), NULL },
		{ NULL}
	};

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	/* TRANSLATORS: A program to load on CCMX correction matrices
	 * onto the hardware */
	context = g_option_context_new (_("ColorHug Refresh"));
	g_option_context_add_group (context, gtk_get_option_group (TRUE));
	g_option_context_add_main_entries (context, options, NULL);
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_warning ("%s: %s", _("Failed to parse command line options"),
			   error->message);
	}
	g_option_context_free (context);

	priv = g_new0 (ChRefreshPrivate, 1);
	priv->settings = g_settings_new ("com.hughski.colorhug-client");
	priv->usb_ctx = g_usb_context_new (NULL);
	priv->measured = g_timer_new ();
	priv->device_list = g_usb_device_list_new (priv->usb_ctx);
	priv->device_queue = ch_device_queue_new ();
	g_signal_connect (priv->device_list, "device-added",
			  G_CALLBACK (ch_refresh_device_added_cb), priv);
	g_signal_connect (priv->device_list, "device-removed",
			  G_CALLBACK (ch_refresh_device_removed_cb), priv);

	/* ensure single instance */
	priv->application = gtk_application_new ("com.hughski.ColorHug.Util", 0);
	g_signal_connect (priv->application, "startup",
			  G_CALLBACK (ch_refresh_startup_cb), priv);
	g_signal_connect (priv->application, "activate",
			  G_CALLBACK (ch_refresh_activate_cb), priv);
	/* set verbose? */
	if (verbose)
		g_setenv ("G_MESSAGES_DEBUG", "ChClient", FALSE);

	/* wait */
	status = g_application_run (G_APPLICATION (priv->application), argc, argv);

	g_object_unref (priv->application);
	if (priv->device_list != NULL)
		g_object_unref (priv->device_list);
	if (priv->device_queue != NULL)
		g_object_unref (priv->device_queue);
	if (priv->usb_ctx != NULL)
		g_object_unref (priv->usb_ctx);
	if (priv->builder != NULL)
		g_object_unref (priv->builder);
	if (priv->settings != NULL)
		g_object_unref (priv->settings);
	if (priv->measured != NULL)
		g_timer_destroy (priv->measured);
	g_free (priv);
	return status;
}
