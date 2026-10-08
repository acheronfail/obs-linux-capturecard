/* SPDX-License-Identifier: AGPL-3.0-only */
#include "toolbar.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <cstring>
#include <obs-frontend-api.h>
#include <obs.h>

namespace {
// OBS 32.2 has no public extension point for the context toolbar. Use named
// Qt widgets and public model data only; don't cast to private OBS classes.
class CaptureToolbar final : public QObject {
  QPointer<QAbstractItemView> sources;
  QPointer<QPushButton> button;
  QTimer timer;

  obs_source_t *selectedCapture() const {
    if (!sources || !sources->selectionModel())
      return nullptr;
    const QModelIndex index = sources->currentIndex();
    if (!index.isValid() || !sources->selectionModel()->isSelected(index))
      return nullptr;
    // SourceTreeModel exposes the full, unelided source name for accessibility.
    const auto name = index.data(Qt::AccessibleTextRole).toString().toUtf8();
    if (name.isEmpty())
      return nullptr;
    obs_source_t *source = obs_get_source_by_name(name.constData());
    if (source &&
        std::strcmp(obs_source_get_unversioned_id(source),
                    "linux_capture_card") == 0 &&
        !obs_source_removed(source))
      return source;
    obs_source_release(source);
    return nullptr;
  }

public:
  CaptureToolbar(QWidget *window, QAbstractItemView *view, QHBoxLayout *layout,
                 int position)
      : QObject(window), sources(view), button(new QPushButton(window)) {
    button->setObjectName("captureCardActivateButton");
    button->setMinimumHeight(22);
    button->setToolTip(
        "Open or close this capture card's video and audio sessions");
    layout->insertWidget(position, button);
    connect(button, &QPushButton::clicked, this, [this] {
      // Resolve again on click: never operate on a previous selection.
      obs_source_t *source = selectedCapture();
      if (source) {
        calldata_t call;
        calldata_init(&call);
        proc_handler_call(obs_source_get_proc_handler(source), "toggle_capture",
                          &call);
        calldata_free(&call);
        obs_source_release(source);
      }
      refresh();
    });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { refresh(); });
    connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { refresh(); });
    // Reflect Properties changes and model replacement/renames without owning
    // a source reference or touching Qt from OBS's capture threads.
    connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
    timer.start(200);
    refresh();
  }

  ~CaptureToolbar() override { delete button.data(); }

  void refresh() {
    if (!button)
      return;
    obs_source_t *source = selectedCapture();
    button->setVisible(source != nullptr);
    if (!source)
      return;
    calldata_t call;
    calldata_init(&call);
    const bool ok = proc_handler_call(obs_source_get_proc_handler(source),
                                      "get_capture_state", &call);
    const bool active = calldata_bool(&call, "active");
    const bool pending = calldata_bool(&call, "pending");
    button->setText(pending ? (active ? "Starting…" : "Stopping…")
                            : (active ? "Deactivate" : "Activate"));
    button->setEnabled(ok && !pending);
    calldata_free(&call);
    obs_source_release(source);
  }
};

QPointer<QObject> controller;
bool registered = false;

void frontend_event(enum obs_frontend_event event, void *) {
  if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && !controller) {
    auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
    controller = capture_toolbar_attach(window);
  } else if (event == OBS_FRONTEND_EVENT_EXIT) {
    delete controller.data();
    controller = nullptr;
  }
}
} // namespace

QObject *capture_toolbar_attach(QWidget *window) {
  if (!window)
    return nullptr;
  auto *view = window->findChild<QAbstractItemView *>("sources");
  auto *container = window->findChild<QWidget *>("contextSubContainer");
  auto *filters = window->findChild<QPushButton *>("sourceFiltersButton");
  auto *layout =
      container ? qobject_cast<QHBoxLayout *>(container->layout()) : nullptr;
  if (!view || !view->selectionModel() || !layout || !filters ||
      layout->indexOf(filters) < 0) {
    blog(LOG_WARNING, "[capture-card] Unsupported OBS toolbar layout; use "
                      "source Properties controls");
    return nullptr;
  }
  return new CaptureToolbar(window, view, layout, layout->indexOf(filters) + 1);
}

extern "C" void capture_toolbar_load(void) {
  // libobs-only tests and embedding applications do not have an OBS frontend.
  if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
    return;
  obs_frontend_add_event_callback(frontend_event, nullptr);
  registered = true;
}

extern "C" void capture_toolbar_unload(void) {
  if (registered) {
    obs_frontend_remove_event_callback(frontend_event, nullptr);
    registered = false;
  }
  delete controller.data();
  controller = nullptr;
}
