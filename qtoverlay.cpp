#include <QApplication>
#include <QRect>
#include <Qt>
#include <gst/video/videooverlay.h>
#include <qpa/qplatformnativeinterface.h>
#include <QDebug>

#include "qtoverlay.h"


struct wl_display *display = NULL;
struct wl_surface *surface = NULL;

#define GST_WAYLAND_DISPLAY_HANDLE_CONTEXT_TYPE "GstWaylandDisplayHandleContextType"

gboolean gst_is_wayland_display_handle_need_context_message(GstMessage *msg) {
    const gchar *type = NULL;

    g_return_val_if_fail(GST_IS_MESSAGE(msg), FALSE);

    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT && gst_message_parse_context_type(msg, &type)) {
        return !g_strcmp0(type, GST_WAYLAND_DISPLAY_HANDLE_CONTEXT_TYPE);
    }

    return FALSE;
}

GstContext *gst_wayland_display_handle_context_new(struct wl_display *display) {
    GstContext *context = gst_context_new(GST_WAYLAND_DISPLAY_HANDLE_CONTEXT_TYPE, TRUE);
    gst_structure_set(gst_context_writable_structure(context), "handle", G_TYPE_POINTER, display, NULL);
    return context;
}

PlayerWindow::PlayerWindow(GstElement *p) : pipeline(p), state(GST_STATE_NULL), totalDuration(GST_CLOCK_TIME_NONE) {
    playBt = new QPushButton("Play");
    pauseBt = new QPushButton("Pause");
    stopBt = new QPushButton("Stop");
    videoWindow = new QWidget();
    slider = new QSlider(Qt::Horizontal);
    timer = new QTimer();

    // debug only, show video window with red background
    QPalette pal(videoWindow->palette());
    pal.setColor(QPalette::Background, Qt::red);
    videoWindow->setAutoFillBackground(true);
    videoWindow->setPalette(pal);

    connect(playBt, SIGNAL(clicked()), this, SLOT(onPlayClicked()));
    connect(pauseBt, SIGNAL(clicked()), this, SLOT(onPauseClicked()));
    connect(stopBt, SIGNAL(clicked()), this, SLOT(onStopClicked()));
    connect(slider, SIGNAL(sliderReleased()), this, SLOT(onSeek()));

    buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(playBt);
    buttonLayout->addWidget(pauseBt);
    buttonLayout->addWidget(stopBt);
    buttonLayout->addWidget(slider);

    playerLayout = new QVBoxLayout;
    playerLayout->addWidget(videoWindow);
    playerLayout->addLayout(buttonLayout);

    this->setLayout(playerLayout);

    connect(timer, SIGNAL(timeout()), this, SLOT(refreshSlider()));
    connect(this, SIGNAL(sigAlbum(QString)), this, SLOT(onAlbumAvaiable(QString)));
    connect(this, SIGNAL(sigState(GstState)), this, SLOT(onState(GstState)));
    connect(this, SIGNAL(sigEos()), this, SLOT(onEos()));
}

WId PlayerWindow::getVideoWId() const { return videoWindow->winId(); }

void PlayerWindow::resizeEvent(QResizeEvent *ev) {
    g_print("resizeEvent: \n");
    g_print("window frameGeometry x: %d, y:%d, w: %d, h: %d\n", this->frameGeometry().x(), this->frameGeometry().y(),
            this->frameGeometry().width(), this->frameGeometry().height());
    g_print("window geometry x: %d, y:%d, w: %d, h: %d\n", this->geometry().x(), this->geometry().y(),
            this->geometry().width(), this->geometry().height());

    QRect vre = this->videoWindow->geometry();
    g_print("vre x: %d, y:%d, w: %d, h: %d \n", vre.x(), vre.y(), vre.width(), vre.height());

    //TODO: resize the gstreamer video overlay 
}

void PlayerWindow::onPlayClicked() {
    GstState st = GST_STATE_NULL;
    gst_element_get_state (GST_ELEMENT(pipeline), &st, NULL, GST_CLOCK_TIME_NONE);
    if (st < GST_STATE_PAUSED) {
        //TODO: should we recreate the pipeline if stopped?
        // Pipeline stopped, we need set overlay again
        // GstElement *vsink = gst_element_factory_make ("wayland", "wsink");
        // g_object_set(GST_OBJECT(pipeline), "video-sink", vsink, NULL);
    }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void PlayerWindow::onPauseClicked() { gst_element_set_state(pipeline, GST_STATE_PAUSED); }

void PlayerWindow::onStopClicked() { gst_element_set_state(pipeline, GST_STATE_NULL); }

void PlayerWindow::onAlbumAvaiable(const QString &album) { setWindowTitle(album); }

void PlayerWindow::onState(GstState st) {
    if (state != st) {
        state = st;
        if (state == GST_STATE_PLAYING) {
            timer->start(1000);
        }
        if (state < GST_STATE_PAUSED) {
            timer->stop();
        }
    }
}

void PlayerWindow::refreshSlider() {
    gint64 current = GST_CLOCK_TIME_NONE;
    if (state == GST_STATE_PLAYING) {
        if (!GST_CLOCK_TIME_IS_VALID(totalDuration)) {
            if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &totalDuration)) {
                slider->setRange(0, totalDuration / GST_SECOND);
            }
        }
        if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &current)) {
            g_print("%ld / %ld\n", current / GST_SECOND, totalDuration / GST_SECOND);
            slider->setValue(current / GST_SECOND);
        }
    }
}

void PlayerWindow::onSeek() {
    gint64 pos = slider->sliderPosition();
    g_print("seek: %ld\n", pos);
    gst_element_seek_simple(pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, pos * GST_SECOND);
}

void PlayerWindow::onEos() { gst_element_set_state(pipeline, GST_STATE_NULL); }

gboolean PlayerWindow::postGstMessage(GstBus *bus, GstMessage *message, gpointer user_data) {
    PlayerWindow *pw = NULL;
    if (user_data) {
        pw = reinterpret_cast<PlayerWindow *>(user_data);
    }
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_STATE_CHANGED: {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);
        pw->sigState(new_state);
        break;
    }
    case GST_MESSAGE_TAG: {
        GstTagList *tags = NULL;
        gst_message_parse_tag(message, &tags);
        gchar *album = NULL;
        if (gst_tag_list_get_string(tags, GST_TAG_ALBUM, &album)) {
            pw->sigAlbum(album);
            g_free(album);
        }
        gst_tag_list_unref(tags);
        break;
    }
    case GST_MESSAGE_EOS: {
        pw->sigEos();
        break;
    }
    case GST_MESSAGE_NEED_CONTEXT:
        g_print("GST_MESSAGE_NEED_CONTEXT\n");

        break;
    default:

        break;
    }
    return TRUE;
}

static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *message, gpointer user_data) {
    PlayerWindow *window = reinterpret_cast<PlayerWindow *>(user_data);

    if (gst_is_wayland_display_handle_need_context_message(message)) {
        g_print("gst_is_wayland_display_handle_need_context_message\n");
        GstContext *context;
        // get the wlayland display handle from Qt
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        display = static_cast<struct wl_display *>(native->nativeResourceForWindow("display", NULL));
        g_print("display: %p\n", display);
        context = gst_wayland_display_handle_context_new(display);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context);

        goto drop;
    } else if (gst_is_video_overlay_prepare_window_handle_message(message)) {
        g_print("gst_is_video_overlay_prepare_window_handle_message\n");
        // we can only get the handle for the top window
        // the handle for videoWindow will be null !! 
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        QWindow *windowHandle = window->windowHandle();
        g_print("top window handle: %p\n", windowHandle);
        surface = static_cast<struct wl_surface *>(native->nativeResourceForWindow("surface", windowHandle));
        g_print("surface: %p\n", surface);
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message)), (guintptr)surface);

        QRect re = window->frameGeometry();
        g_print("window rect x: %d, y:%d, w:%d, h:%d\n", re.x(), re.y(), re.width(), re.height());

        QRect vre = window->videoWindow->geometry();
        g_print("video window rect x: %d, y:%d, w: %d, h: %d \n", vre.x(), vre.y(), vre.width(), vre.height());
        
        gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message)), 
                vre.x() - re.x(), vre.y() - re.y(), vre.width(), vre.height());

        goto drop;
    }

    return GST_BUS_PASS;

drop:
    gst_message_unref(message);
    return GST_BUS_DROP;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    QApplication app(argc, argv);
    app.connect(&app, SIGNAL(lastWindowClosed()), &app, SLOT(quit()));

    qDebug() << "qt platform name: " << QGuiApplication::platformName();
    // prepare the pipeline
    GstElement *pipeline = gst_parse_launch("playbin uri=https://www.freedesktop.org/software/gstreamer-sdk/data/media/sintel_trailer-480p.webm", NULL);

    // prepare the ui
    PlayerWindow *window = new PlayerWindow(pipeline);
    window->resize(640, 480);
    window->show();

    GstElement *vsink = gst_element_factory_make("waylandsink", "wsink");
    g_object_set(GST_OBJECT(pipeline), "video-sink", vsink, NULL);

    // connect to interesting signals
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, &PlayerWindow::postGstMessage, window);
    gst_bus_set_sync_handler(bus, bus_sync_handler, window, NULL);
    gst_object_unref(bus);

    // run the pipeline
    GstStateChangeReturn sret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        // Exit application
        QTimer::singleShot(0, QApplication::activeWindow(), SLOT(quit()));
    }

    int ret = app.exec();

    window->hide();
    delete window;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return ret;
}
