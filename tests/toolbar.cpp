#include "toolbar.hpp"
#include <QApplication>
#include <QHBoxLayout>
#include <QListView>
#include <QPointer>
#include <QPushButton>
#include <QStandardItemModel>
#include <QThread>
#include <cassert>
#include <obs.h>

struct State {
  bool active = true;
  bool pending = false;
};
static const char *name(void *) { return "Test"; }
static void get_state(void *data, calldata_t *call) {
  calldata_set_bool(call, "active", static_cast<State *>(data)->active);
  calldata_set_bool(call, "pending", static_cast<State *>(data)->pending);
}
static void set_pending(void *data, calldata_t *call) {
  static_cast<State *>(data)->pending = calldata_bool(call, "pending");
}
static void toggle(void *data, calldata_t *) {
  auto *state = static_cast<State *>(data);
  state->active = !state->active;
}
static void *create(obs_data_t *, obs_source_t *source) {
  auto *state = new State;
  proc_handler_add(obs_source_get_proc_handler(source),
                   "void get_capture_state(out bool active, out bool pending)",
                   get_state, state);
  proc_handler_add(obs_source_get_proc_handler(source), "void toggle_capture()",
                   toggle, state);
  proc_handler_add(obs_source_get_proc_handler(source),
                   "void set_pending(in bool pending)", set_pending, state);
  return state;
}
static void destroy(void *data) { delete static_cast<State *>(data); }
static bool active(obs_source_t *source) {
  calldata_t call;
  calldata_init(&call);
  assert(proc_handler_call(obs_source_get_proc_handler(source),
                           "get_capture_state", &call));
  bool result = calldata_bool(&call, "active");
  calldata_free(&call);
  return result;
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  assert(obs_startup("en-US", nullptr, nullptr));
  obs_source_info info = {};
  info.id = "linux_capture_card";
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.get_name = name;
  info.create = create;
  info.destroy = destroy;
  obs_register_source(&info);
  auto *first = obs_source_create(info.id, "First capture", nullptr, nullptr);
  auto *second = obs_source_create(info.id, "Second capture", nullptr, nullptr);
  info.id = "other_source";
  obs_register_source(&info);
  auto *other = obs_source_create(info.id, "Other source", nullptr, nullptr);
  QWidget window;
  auto *view = new QListView(&window);
  view->setObjectName("sources");
  QStandardItemModel model;
  for (const auto &text : {"First capture", "Second capture", "Other source"}) {
    auto *item = new QStandardItem("Elided label");
    item->setData(QString(text), Qt::AccessibleTextRole);
    model.appendRow(item);
  }
  view->setModel(&model);
  auto *container = new QWidget(&window);
  container->setObjectName("contextSubContainer");
  auto *layout = new QHBoxLayout(container);
  auto *properties = new QPushButton("Properties", container);
  layout->addWidget(properties);
  auto *filters = new QPushButton("Filters", container);
  filters->setObjectName("sourceFiltersButton");
  layout->addWidget(filters);
  QObject *controller = capture_toolbar_attach(&window);
  assert(controller);
  QPointer<QPushButton> button =
      window.findChild<QPushButton *>("captureCardActivateButton");
  assert(button && button->isHidden());
  assert(layout->indexOf(button) == layout->indexOf(filters) + 1);
  view->setCurrentIndex(model.index(0, 0));
  assert(!button->isHidden() && button->text() == "Deactivate");
  button->click();
  assert(!active(first) && active(second) && button->text() == "Activate");
  // Rapidly switch source before the periodic refresh; clicking must use the
  // new one.
  view->setCurrentIndex(model.index(1, 0));
  button->click();
  assert(!active(first) && !active(second));
  button->click();
  assert(active(second));
  // Changes from the Properties dialog are reflected by the state refresh.
  calldata_t call;
  calldata_init(&call);
  proc_handler_call(obs_source_get_proc_handler(second), "toggle_capture",
                    &call);
  calldata_free(&call);
  QThread::msleep(250);
  app.processEvents();
  assert(button->text() == "Activate");
  calldata_init(&call);
  calldata_set_bool(&call, "pending", true);
  proc_handler_call(obs_source_get_proc_handler(second), "set_pending", &call);
  QThread::msleep(250);
  app.processEvents();
  assert(!button->isEnabled() && button->text() == "Stopping…");
  proc_handler_call(obs_source_get_proc_handler(second), "toggle_capture",
                    &call);
  QThread::msleep(250);
  app.processEvents();
  assert(!button->isEnabled() && button->text() == "Starting…");
  calldata_set_bool(&call, "pending", false);
  proc_handler_call(obs_source_get_proc_handler(second), "set_pending", &call);
  QThread::msleep(250);
  app.processEvents();
  assert(button->isEnabled() && button->text() == "Deactivate");
  calldata_free(&call);
  view->setCurrentIndex(model.index(2, 0));
  assert(button->isHidden());
  button->click();
  assert(active(other));
  view->clearSelection();
  assert(button->isHidden());
  delete controller;
  assert(!button);
  QWidget incompatible;
  assert(capture_toolbar_attach(&incompatible) == nullptr);
  obs_source_release(first);
  obs_source_release(second);
  obs_source_release(other);
  obs_shutdown();
  return 0;
}
