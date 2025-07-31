#include "track_controller.hpp"
#include "file_controller.hpp"
#include "glib-object.h"
#include "glib.h"
#include "gst/gstbin.h"
#include "gst/gstbus.h"
#include "gst/gstclock.h"
#include "gst/gstelement.h"
#include "gst/gstelementfactory.h"
#include "gst/gstenumtypes.h"
#include "gst/gstformat.h"
#include "gst/gstmessage.h"
#include "gst/gstpad.h"
#include "gst/gstpipeline.h"
#include "gst/gstsegment.h"
#include "gst/gststructure.h"
#include "gst/gstutils.h"
#include "gst/gstvalue.h"
#include "sigc++/functors/mem_fun.h"

#include <concepts>
#include <format>
#include <glibmm/random.h>
#include <iostream>
#include <memory>
#include <string>
#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <vector>
track_controller::track_controller(track_list_pane *track_list_view) {
  this->track_list_view = track_list_view;
  elements.playing = false;
  elements.terminate = false;
  elements.seek_enabled = false;
  elements.seek_done = false;
  elements.duration = GST_CLOCK_TIME_NONE;
  elements.source = gst_element_factory_make("playbin3", "audio_src");
  g_object_set(elements.source, "volume", 0.5, nullptr);
  bus = gst_element_get_bus(elements.source);
  track_list_view->track_list_view->signal_activate().connect(
      sigc::mem_fun(*this, &track_controller::on_column_selected));

  g_signal_connect(elements.source, "about_to_finish",
                   G_CALLBACK(end_of_stream_callback), this);
}
void track_controller::handle_message(std::shared_ptr<track_controller> data,
                                      GstMessage *msg) {
  GError *err;
  gchar *debug_info;
  switch (GST_MESSAGE_TYPE(msg)) {
  case GST_MESSAGE_BUFFERING: {
    gint percent = 0;
    gst_message_parse_buffering(msg, &percent);
    g_print("Buffering (%u percent done)", percent);
    break;
  }
  case GST_MESSAGE_STREAM_START: {
    data->elements.playing = true;
    data->stopped_state = false;
    data->track_list_view->track_list_view->get_model()->select_item(
        data->playing_track_index, data->unselect_rest);
    data->playing_state_label();
    break;
  }
  case GST_MESSAGE_ERROR:
    gst_message_parse_error(msg, &err, &debug_info);
    g_printerr("Error received from element %s: %s\n",
               GST_OBJECT_NAME(msg->src), err->message);
    g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
    g_clear_error(&err);
    g_free(debug_info);
    data->elements.terminate = TRUE;
    break;
  case GST_MESSAGE_EOS: {
    g_print("\nEnd-Of-Stream reached.\n");
    data->elements.terminate = TRUE;

    break;
  }
  case GST_MESSAGE_DURATION_CHANGED:
    data->elements.duration = GST_CLOCK_TIME_NONE;
    std::cout << "duration changed" << std::endl;
    break;
  case GST_MESSAGE_STATE_CHANGED: {
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed(msg, &old_state, &new_state,
                                    &pending_state);
    if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->elements.source)) {
      g_print("Pipeline state changed from %s to %s:\n",
              gst_element_state_get_name(old_state),
              gst_element_state_get_name(new_state));
      data->elements.playing = (new_state == GST_STATE_PLAYING);
    }
  } break;
  }
}
void track_controller::pad_added_handler(GstElement *src, GstPad *new_pad,
                                         player *data) {
  GstPad *sink_pad = gst_element_get_static_pad(data->convert, "sink");
  GstPadLinkReturn ret;
  GstCaps *new_pad_caps = nullptr;
  GstStructure *new_pad_struct = nullptr;
  const gchar *new_pad_type = nullptr;
  const gchar *pad_name = gst_pad_get_name(new_pad);
  g_print("Received new pad '%s' from '%s':\n", pad_name,
          GST_ELEMENT_NAME(src));
  if (!g_str_has_prefix(pad_name, "audio_")) {
    g_print("Ignoring pad '%s' as it is not audio.\n", pad_name);
    g_free((gchar *)pad_name);
    return;
  }
  new_pad_caps = gst_pad_query_caps(new_pad, nullptr);
  if (!new_pad_caps || gst_caps_is_empty(new_pad_caps)) {
    g_print("No caps available or caps are empty for "
            "pad '%s'. Ignoring.\n",
            pad_name);
    if (new_pad_caps)
      gst_caps_unref(new_pad_caps);
    g_free((gchar *)pad_name);
    return;
  }
  new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
  new_pad_type = gst_structure_get_name(new_pad_struct);
  if (!g_str_has_prefix(new_pad_type, "audio/x-raw")) {
    g_print("Pad '%s' has type '%s' which is not raw "
            "audio. Ignoring.\n",
            pad_name, new_pad_type);
    gst_caps_unref(new_pad_caps);
    g_free((gchar *)pad_name);
    return;
  }
  ret = gst_pad_link(new_pad, sink_pad);
  if (GST_PAD_LINK_FAILED(ret)) {
    g_print("Linking pad '%s' with type '%s' failed.\n", pad_name,
            new_pad_type);
  } else {
    g_print("Successfully linked pad '%s' (type '%s').\n", pad_name,
            new_pad_type);
  }
  gst_caps_unref(new_pad_caps);
  g_free((gchar *)pad_name);
}
void track_controller::on_changed_volume() {
  g_object_set(elements.source, "volume", track_list_view->slider->get_value(),
               nullptr);
}
void track_controller::update_playing_buttons() {
  if (elements.playing) {
    track_list_view->play_button->set_label("⏸︎");
  } else
    track_list_view->play_button->set_label("▶");
  track_list_view->update();
}
void track_controller::play() {
  std::shared_ptr<Gtk::SingleSelection> ss =
      std::dynamic_pointer_cast<Gtk::SingleSelection>(
          track_list_view->track_list_view->get_model());
  if (ss->get_selected() != playing_track_index) {
    std::cout << "asdasdasdasd" << std::endl;
    playing_track_index = ss->get_selected();
    std::string path_with_index = get_path_of_column(ss->get_selected());
    g_object_set(elements.source, "uri", path_with_index.c_str(), NULL);
    gst_element_set_state(elements.source, GST_STATE_NULL);
    gst_element_set_state(elements.source, GST_STATE_PLAYING);
  } else if (!elements.playing) {
    elements.playing = true;
    gst_element_set_state(elements.source, GST_STATE_PLAYING);

  } else {
    elements.playing = false;
    gst_element_set_state(elements.source, GST_STATE_PAUSED);
  }
  playing_state_label();
  update_playing_buttons();
  stopped_state = false;
}
void track_controller::stop() {
  gst_element_set_state(elements.source, GST_STATE_NULL);
  stopped_state = true;
  elements.playing = false;
  track_list_view->update();
  playing_state_label();
  update_playing_buttons();
}
void track_controller::add_track(const std::string &path) {
  TagLib::FileRef file_add(path.c_str());
  Glib::ustring artist = file_add.tag()->artist().to8Bit();
  Glib::ustring song_name = file_add.tag()->title().to8Bit();
  Glib::ustring album = file_add.tag()->album().to8Bit();
  Glib::ustring track_id = std::to_string(file_add.tag()->track());
  std::string new_file_path = "file://" + path;
  g_object_set(elements.source, "uri", new_file_path.c_str(), NULL);
  std::string duration_mins = std::format(
      "{:02}", (file_add.file()->audioProperties()->lengthInSeconds() / 60));
  std::string duration_secs = std::format(
      "{:02}", (file_add.file()->audioProperties()->lengthInSeconds() % 60));
  track_list_view->track_model->append(
      track_item::create("", track_id, artist, song_name, album,
                         duration_mins + ":" + duration_secs));
}
void track_controller::on_track_selected() {}

std::string track_controller::get_path_of_column(int index) {
  return column_path[index];
}
void track_controller::on_column_selected(guint pos) {
  std::shared_ptr<Gtk::SingleSelection> ss =
      std::dynamic_pointer_cast<Gtk::SingleSelection>(
          track_list_view->track_list_view->get_model());
  std::string path_with_index = get_path_of_column(ss->get_selected());
  g_object_set(elements.source, "uri", path_with_index.c_str(), NULL);
  std::cout << path_with_index << std::endl;
  gst_element_set_state(elements.source, GST_STATE_NULL);
  gst_element_set_state(elements.source, GST_STATE_PLAYING);
  playing_track_index = ss->get_selected();
  elements.playing = true;
  stopped_state = false;
  update_playing_buttons();
  playing_state_label();
}
void track_controller::playing_state_label() {
  std::shared_ptr<Gtk::SingleSelection> st =
      std::dynamic_pointer_cast<Gtk::SingleSelection>(
          track_list_view->track_list_view->get_model());
  for (int i = 0; i < column_path.size(); i++) {
    track_list_view->track_model->get_item(i)->playing = "";
    track_list_view->update();
  }
  if (track_list_view->track_model->get_item(0)) {
    if (!elements.playing) {
      track_list_view->track_model->get_item(st->get_selected())->playing = "▶";
      track_list_view->update();
    } else {
      track_list_view->track_model->get_item(st->get_selected())->playing = "⏸︎";
      track_list_view->update();
    }
  } else {
    std::cout << "No File Selected" << std::endl;
  }
}
void track_controller::previous() {
  if (playing_track_index - 1 >= 0) {
    elements.playing = true;
    stopped_state = false;
    previous_selection();
    random_selection();
    gst_element_set_state(elements.source, GST_STATE_NULL);
    gst_element_set_state(elements.source, GST_STATE_PLAYING);
    update_playing_buttons();
    playing_state_label();
  } else {
    std::cout << "No previous track" << std::endl;
  }
}
void track_controller::next() {
  if (playing_track_index + 1 <= column_path.size()) {
    random_selection();
    gst_element_set_state(elements.source, GST_STATE_NULL);
    gst_element_set_state(elements.source, GST_STATE_PLAYING);
    update_playing_buttons();
    playing_state_label();
  } else if (playing_track_index + 1 < column_path.size()) {
    elements.playing = true;
    stopped_state = false;
    random_selection();
    loop_selection();
    gst_element_set_state(elements.source, GST_STATE_NULL);
    gst_element_set_state(elements.source, GST_STATE_PLAYING);
    update_playing_buttons();
    playing_state_label();
  }
}
void track_controller::end_of_stream_callback(GstElement *src,
                                              track_controller *controller,
                                              GstMessage *msg) {
  Glib::Rand random_number_uri;
  std::string path_with_index;
  if (controller->track_list_view->dropdown_button->get_selected() == 0) {
    controller->playing_track_index += 1;
    path_with_index =
        controller->get_path_of_column(controller->playing_track_index);
    g_object_set(controller->elements.source, "uri", path_with_index.c_str(),
                 NULL);
  } else if (controller->track_list_view->dropdown_button->get_selected() ==
             1) {
    controller->random_index =
        random_number_uri.get_int_range(0, controller->column_path.size());
    controller->playing_track_index = controller->random_index;
    path_with_index =
        controller->get_path_of_column(controller->playing_track_index);
    g_object_set(controller->elements.source, "uri", path_with_index.c_str(),
                 NULL);
    controller->playing_track_index = controller->random_index;
  } else if (controller->track_list_view->dropdown_button->get_selected() ==
             2) {
    path_with_index =
        controller->get_path_of_column(controller->playing_track_index);
    g_object_set(controller->elements.source, "uri", path_with_index.c_str(),
                 NULL);
  }
};
void track_controller::random_selection() {
  random_index = playing_track_index;
  while (random_index == playing_track_index) {
    random_index = random_number.get_int_range(0, column_path.size());
  }
  if (track_list_view->dropdown_button->get_selected() == 1) {
    track_list_view->track_list_view->get_model()->select_item(random_index,
                                                               unselect_rest);
    playing_track_index = random_index;
    std::string path_with_index = get_path_of_column(random_index);
    g_object_set(elements.source, "uri", path_with_index.c_str(), NULL);
  }
}
void track_controller::loop_selection() {
  if (track_list_view->dropdown_button->get_selected() == 0 ||
      track_list_view->dropdown_button->get_selected() == 2) {
    playing_track_index += 1;

    track_list_view->track_list_view->get_model()->select_item(
        playing_track_index, unselect_rest);
    std::string path_with_index = get_path_of_column(playing_track_index);
    g_object_set(elements.source, "uri", path_with_index.c_str(), NULL);
  }
}
void track_controller::previous_selection() {
  if (track_list_view->dropdown_button->get_selected() == 0 ||
      track_list_view->dropdown_button->get_selected() == 2) {
    playing_track_index -= 1;
    track_list_view->track_list_view->get_model()->select_item(
        playing_track_index, unselect_rest);
    std::string path_with_index = get_path_of_column(playing_track_index);
    g_object_set(elements.source, "uri", path_with_index.c_str(), NULL);
  }
}
