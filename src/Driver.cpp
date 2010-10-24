// ekam -- http://code.google.com/p/ekam
// Copyright (c) 2010 Kenton Varda and contributors.  All rights reserved.
// Portions copyright Google, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of the ekam project nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "Driver.h"

#include <queue>
#include <tr1/memory>
#include <stdexcept>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "Debug.h"
#include "EventGroup.h"

namespace ekam {

namespace {

int fileDepth(const std::string& name) {
  int result = 0;
  for (unsigned int i = 0; i < name.size(); i++) {
    if (name[i] == '/') {
      ++result;
    }
  }
  return result;
}

int commonPrefixLength(const std::string& srcName, const std::string& bestMatchName) {
  std::string::size_type n = std::min(srcName.size(), bestMatchName.size());
  for (unsigned int i = 0; i < n; i++) {
    if (srcName[i] != bestMatchName[i]) {
      return i;
    }
  }
  return n;
}

}  // namespace

class Driver::ActionDriver : public BuildContext, public EventGroup::ExceptionHandler {
public:
  ActionDriver(Driver* driver, OwnedPtr<Action>* actionToAdopt,
               File* srcfile, Hash srcHash, OwnedPtr<Dashboard::Task>* taskToAdopt);
  ~ActionDriver();

  void start();

  // implements BuildContext -------------------------------------------------------------
  File* findProvider(Tag id);
  File* findInput(const std::string& path);

  void provide(File* file, const std::vector<Tag>& tags);
  void log(const std::string& text);

  void newOutput(const std::string& path, OwnedPtr<File>* output);

  void addActionType(OwnedPtr<ActionFactory>* factoryToAdopt);

  void passed();
  void failed();

  // implements ExceptionHandler ---------------------------------------------------------
  void threwException(const std::exception& e);
  void threwUnknownException();
  void noMoreEvents();

private:
  class StartCallback : public EventManager::Callback {
  public:
    StartCallback(ActionDriver* actionDriver) : actionDriver(actionDriver) {}
    ~StartCallback() {}

    // implements Callback ---------------------------------------------------------------
    void run() {
      actionDriver->asyncCallbackOp.clear();
      actionDriver->action->start(&actionDriver->eventGroup, actionDriver,
                                  &actionDriver->runningAction);
    }

  private:
    ActionDriver* actionDriver;
  };

  class DoneCallback : public EventManager::Callback {
  public:
    DoneCallback(ActionDriver* actionDriver) : actionDriver(actionDriver) {}
    ~DoneCallback() {}

    // implements Callback ---------------------------------------------------------------
    void run() {
      actionDriver->asyncCallbackOp.clear();
      Driver* driver = actionDriver->driver;
      actionDriver->returned();  // may delete actionDriver
      driver->startSomeActions();
    }

  private:
    ActionDriver* actionDriver;
  };


  Driver* driver;
  OwnedPtr<Action> action;
  OwnedPtr<File> srcfile;
  Hash srcHash;
  OwnedPtr<Dashboard::Task> dashboardTask;

  // TODO:  Get rid of "state".  Maybe replace with "status" or something, but don't try to
  //   track both whether we're running and what the status was at the same time.  (I already
  //   had to split isRunning into a separate boolean due to issues with this.)
  enum {
    PENDING,
    RUNNING,
    DONE,
    PASSED,
    FAILED
  } state;

  EventGroup eventGroup;

  StartCallback startCallback;
  DoneCallback doneCallback;
  OwnedPtr<AsyncOperation> asyncCallbackOp;

  bool isRunning;
  OwnedPtr<AsyncOperation> runningAction;

  OwnedPtrVector<File> outputs;

  OwnedPtrVector<Provision> provisions;
  OwnedPtrVector<std::vector<Tag> > providedTags;

  void ensureRunning();
  void queueDoneCallback();
  void returned();
  void reset();
  Provision* choosePreferredProvider(const Tag& tag);

  friend class Driver;
};

Driver::ActionDriver::ActionDriver(Driver* driver, OwnedPtr<Action>* actionToAdopt,
                                   File* srcfile, Hash srcHash,
                                   OwnedPtr<Dashboard::Task>* taskToAdopt)
    : driver(driver), srcHash(srcHash), state(PENDING),
      eventGroup(driver->eventManager, this), startCallback(this), doneCallback(this),
      isRunning(false) {
  action.adopt(actionToAdopt);
  srcfile->clone(&this->srcfile);

  dashboardTask.adopt(taskToAdopt);
}
Driver::ActionDriver::~ActionDriver() {}

void Driver::ActionDriver::start() {
  if (state != PENDING) {
    DEBUG_ERROR << "State must be PENDING here.";
  }
  assert(!driver->dependencyTable.has<DependencyTable::ACTION>(this));
  assert(outputs.empty());
  assert(provisions.empty());
  assert(!isRunning);

  state = RUNNING;
  isRunning = true;
  dashboardTask->setState(Dashboard::RUNNING);

  OwnedPtr<EventManager::Callback> callback;
  eventGroup.runAsynchronously(&startCallback, &asyncCallbackOp);
}

File* Driver::ActionDriver::findProvider(Tag tag) {
  ensureRunning();

  Provision* provision = choosePreferredProvider(tag);

  if (provision == NULL) {
    driver->dependencyTable.add(tag, this, NULL);
    return NULL;
  } else {
    driver->dependencyTable.add(tag, this, provision);
    return provision->file.get();
  }
}

File* Driver::ActionDriver::findInput(const std::string& path) {
  ensureRunning();

  return findProvider(Tag::fromFile(path));
}

void Driver::ActionDriver::provide(File* file, const std::vector<Tag>& tags) {
  ensureRunning();

  // Find existing provision for this file, if any.
  // TODO:  Convert provisions into a map?
  Provision* provision = NULL;
  for (int i = 0; i < provisions.size(); i++) {
    if (provisions.get(i)->file->equals(file)) {
      provision = provisions.get(i);
      providedTags.get(i)->insert(providedTags.get(i)->end(), tags.begin(), tags.end());
      break;
    }
  }

  if (provision == NULL) {
    OwnedPtr<Provision> ownedProvision;
    ownedProvision.allocate();
    provision = ownedProvision.get();
    provisions.adoptBack(&ownedProvision);

    OwnedPtr<std::vector<Tag> > tagsCopy;
    tagsCopy.allocate(tags);
    providedTags.adoptBack(&tagsCopy);
  }

  file->clone(&provision->file);
}

void Driver::ActionDriver::log(const std::string& text) {
  ensureRunning();
  dashboardTask->addOutput(text);
}

void Driver::ActionDriver::newOutput(const std::string& path, OwnedPtr<File>* output) {
  ensureRunning();
  OwnedPtr<File> file;
  driver->tmp->relative(path, &file);

  OwnedPtr<File> parent;
  file->parent(&parent);
  recursivelyCreateDirectory(parent.get());

  file->clone(output);

  std::vector<Tag> tags;
  tags.push_back(Tag::DEFAULT_TAG);
  provide(file.get(), tags);

  outputs.adoptBack(&file);
}

void Driver::ActionDriver::addActionType(OwnedPtr<ActionFactory>* factoryToAdopt) {
  ensureRunning();
  driver->addActionFactory(factoryToAdopt->get());
  driver->rescanForNewFactory(factoryToAdopt->get());
  driver->ownedFactories.adoptBack(factoryToAdopt);
}

void Driver::ActionDriver::noMoreEvents() {
  ensureRunning();

  if (state == RUNNING) {
    state = DONE;
    queueDoneCallback();
  }
}

void Driver::ActionDriver::passed() {
  ensureRunning();

  if (state == FAILED) {
    // Ignore passed() after failed().
    return;
  }

  state = PASSED;
  queueDoneCallback();
}

void Driver::ActionDriver::failed() {
  ensureRunning();

  if (state == FAILED) {
    // Ignore redundant call to failed().
    return;
  } else if (state == DONE) {
    // (done callback should already be queued)
    throw std::runtime_error("Called failed() after success().");
  } else if (state == PASSED) {
    // (done callback should already be queued)
    throw std::runtime_error("Called failed() after passed().");
  } else {
    state = FAILED;
    queueDoneCallback();
  }
}

void Driver::ActionDriver::ensureRunning() {
  if (!isRunning) {
    throw std::runtime_error("Action is not running.");
  }
}

void Driver::ActionDriver::queueDoneCallback() {
  driver->eventManager->runAsynchronously(&doneCallback, &asyncCallbackOp);
}

void Driver::ActionDriver::threwException(const std::exception& e) {
  ensureRunning();
  dashboardTask->addOutput(std::string("uncaught exception: ") + e.what() + "\n");
  asyncCallbackOp.clear();
  state = FAILED;
  returned();
}

void Driver::ActionDriver::threwUnknownException() {
  ensureRunning();
  dashboardTask->addOutput("uncaught exception of unknown type\n");
  asyncCallbackOp.clear();
  state = FAILED;
  returned();
}

void Driver::ActionDriver::returned() {
  ensureRunning();

  // Cancel anything still running.
  runningAction.clear();
  isRunning = false;

  // Pull self out of driver->activeActions.
  OwnedPtr<ActionDriver> self;
  for (int i = 0; i < driver->activeActions.size(); i++) {
    if (driver->activeActions.get(i) == this) {
      driver->activeActions.releaseAndShift(i, &self);
      break;
    }
  }

  driver->completedActionPtrs.adopt(this, &self);

  if (state == FAILED) {
    // Failed, possibly due to missing dependencies.
    provisions.clear();
    providedTags.clear();
    outputs.clear();
    dashboardTask->setState(Dashboard::BLOCKED);
  } else {
    dashboardTask->setState(state == PASSED ? Dashboard::PASSED : Dashboard::DONE);

    // Remove outputs which were deleted before the action completed.  Some actions create
    // files and then delete them immediately.
    OwnedPtrVector<Provision> provisionsToFilter;
    provisions.swap(&provisionsToFilter);
    for (int i = 0; i < provisionsToFilter.size(); i++) {
      if (provisionsToFilter.get(i)->file->exists()) {
        OwnedPtr<Provision> temp;
        provisionsToFilter.release(i, &temp);
        provisions.adoptBack(&temp);
      }
    }

    // Register providers.
    for (int i = 0; i < provisions.size(); i++) {
      driver->registerProvider(provisions.get(i), *providedTags.get(i));
    }
    providedTags.clear();  // Not needed anymore.
  }
}

void Driver::ActionDriver::reset() {
  if (state == PENDING) {
    // Nothing to do.
    return;
  }

  OwnedPtr<ActionDriver> self;

  if (isRunning) {
    dashboardTask->setState(Dashboard::BLOCKED);
    runningAction.clear();
    asyncCallbackOp.clear();

    for (int i = 0; i < driver->activeActions.size(); i++) {
      if (driver->activeActions.get(i) == this) {
        driver->activeActions.releaseAndShift(i, &self);
        break;
      }
    }

    isRunning = false;
  } else {
    if (!driver->completedActionPtrs.release(this, &self)) {
      throw std::logic_error("Action not running or pending, but not in completedActionPtrs?");
    }
  }

  state = PENDING;

  // Put on back of queue (as opposed to front) so that actions which are frequently reset
  // don't get redundantly rebuilt too much.  We add the action to the queue before resetting
  // dependents so that this action gets re-run before its dependents.
  // TODO:  The second point probably doesn't help much when multiprocessing.  Maybe the
  //   action queue should really be a graph that remembers what depended on what the last
  //   time we ran them, and avoids re-running any action before re-running actions on which it
  //   depended last time.
  driver->pendingActions.adoptBack(&self);

  // Reset dependents.
  for (int i = 0; i < provisions.size(); i++) {
    Provision* provision = provisions.get(i);

    // Reset dependents of this provision.
    {
      std::vector<ActionDriver*> actionsToReset;
      for (DependencyTable::SearchIterator<DependencyTable::PROVISION>
           iter(driver->dependencyTable, provision); iter.next();) {
        // Can't call reset() directly here because it may invalidate our iterator.
        actionsToReset.push_back(iter.cell<DependencyTable::ACTION>());
      }
      for (size_t j = 0; j < actionsToReset.size(); j++) {
        actionsToReset[j]->reset();
      }
    }

    // Everything triggered by this provision must be deleted.
    {
      std::vector<ActionDriver*> actionsToDelete;
      std::pair<ActionsByTriggerMap::iterator, ActionsByTriggerMap::iterator> range =
          driver->actionsByTrigger.equal_range(provision);
      for (ActionsByTriggerMap::iterator iter = range.first; iter != range.second; ++iter) {
        // Can't call reset() directly here because it may invalidate our iterator.
        actionsToDelete.push_back(iter->second);
      }
      driver->actionsByTrigger.erase(range.first, range.second);

      for (size_t j = 0; j < actionsToDelete.size(); j++) {
        actionsToDelete[j]->reset();

        // TODO:  Use better data structure for pendingActions.  For now we have to iterate
        //   through the whole thing to find the action we're deleting.  We iterate from the back
        //   since it's likely the action was just added there.
        OwnedPtr<ActionDriver> ownedAction;
        for (int k = driver->pendingActions.size() - 1; k >= 0; k--) {
          if (driver->pendingActions.get(k) == actionsToDelete[j]) {
            driver->pendingActions.releaseAndShift(k, &ownedAction);
            break;
          }
        }
      }
    }

    driver->tagTable.erase<TagTable::PROVISION>(provision);
    if (driver->dependencyTable.erase<DependencyTable::PROVISION>(provision) > 0) {
      DEBUG_ERROR << "Resetting dependents should have removed this provision from "
                     "dependencyTable.";
    }
  }

  // Remove all entries in dependencyTable pointing at this action.
  driver->dependencyTable.erase<DependencyTable::ACTION>(this);

  provisions.clear();
  providedTags.clear();
  outputs.clear();
}

Driver::Provision* Driver::ActionDriver::choosePreferredProvider(const Tag& tag) {
  TagTable::SearchIterator<TagTable::TAG> iter(driver->tagTable, tag);

  if (!iter.next()) {
    return NULL;
  } else {
    std::string srcName = srcfile->canonicalName();
    Provision* bestMatch = iter.cell<TagTable::PROVISION>();

    if (iter.next()) {
      // There are multiple files with this tag.  We must choose which one we like best.
      std::string bestMatchName = bestMatch->file->canonicalName();
      int bestMatchDepth = fileDepth(bestMatchName);
      int bestMatchCommonPrefix = commonPrefixLength(srcName, bestMatchName);

      do {
        Provision* candidate = iter.cell<TagTable::PROVISION>();
        std::string candidateName = candidate->file->canonicalName();
        int candidateDepth = fileDepth(candidateName);
        int candidateCommonPrefix = commonPrefixLength(srcName, candidateName);
        if (candidateCommonPrefix < bestMatchCommonPrefix) {
          // Prefer provider that is closer in the directory tree.
          continue;
        } else if (candidateCommonPrefix == bestMatchCommonPrefix) {
          if (candidateDepth > bestMatchDepth) {
            // Prefer provider that is less deeply nested.
            continue;
          } else if (candidateDepth == bestMatchDepth) {
            // Arbitrarily -- but consistently -- choose one.
            int diff = bestMatchName.compare(candidateName);
            if (diff < 0) {
              // Prefer file that comes first alphabetically.
              continue;
            } else if (diff == 0) {
              // TODO:  Is this really an error?  I think it is for the moment, but someday it
              //   may not be, if multiple actions are allowed to produce outputs with the same
              //   canonical names.
              DEBUG_ERROR << "Two providers have same file name: " << bestMatchName;
              continue;
            }
          }
        }

        // If we get here, the candidate is better than the existing best match.
        bestMatch = candidate;
        bestMatchName.swap(candidateName);
        bestMatchDepth = candidateDepth;
        bestMatchCommonPrefix = candidateCommonPrefix;
      } while(iter.next());
    }

    return bestMatch;
  }
}

// =======================================================================================

Driver::Driver(EventManager* eventManager, Dashboard* dashboard, File* src, File* tmp,
               int maxConcurrentActions)
    : eventManager(eventManager), dashboard(dashboard), src(src), tmp(tmp),
      maxConcurrentActions(maxConcurrentActions) {
  if (!tmp->exists()) {
    tmp->createDirectory();
  }
}

Driver::~Driver() {
  // Error out all blocked tasks.
  for (OwnedPtrMap<ActionDriver*, ActionDriver>::Iterator iter(completedActionPtrs); iter.next();) {
    if (iter.key()->state == ActionDriver::FAILED) {
      iter.value()->dashboardTask->setState(Dashboard::FAILED);
    }
  }
}

void Driver::addActionFactory(ActionFactory* factory) {
  std::vector<Tag> triggerTags;
  factory->enumerateTriggerTags(std::back_inserter(triggerTags));
  for (unsigned int i = 0; i < triggerTags.size(); i++) {
    triggers.insert(std::make_pair(triggerTags[i], factory));
  }
}

void Driver::start() {
  scanSourceTree();
  startSomeActions();
}

void Driver::startSomeActions() {
  while (activeActions.size() < maxConcurrentActions && !pendingActions.empty()) {
    OwnedPtr<ActionDriver> actionDriver;
    pendingActions.releaseFront(&actionDriver);
    ActionDriver* ptr = actionDriver.get();
    activeActions.adoptBack(&actionDriver);
    try {
      ptr->start();
    } catch (const std::exception& e) {
      ptr->threwException(e);
    } catch (...) {
      ptr->threwUnknownException();
    }
  }
}

void Driver::scanSourceTree() {
  OwnedPtrVector<File> fileQueue;

  {
    OwnedPtr<File> root;
    src->clone(&root);
    fileQueue.adoptBack(&root);
  }

  while (!fileQueue.empty()) {
    OwnedPtr<File> current;
    fileQueue.releaseBack(&current);

    if (current->isDirectory()) {
      OwnedPtrVector<File> list;
      current->list(list.appender());
      for (int i = 0; i < list.size(); i++) {
        OwnedPtr<File> child;
        list.release(i, &child);
        fileQueue.adoptBack(&child);
      }
    } else {
      // Apply default tag.
      OwnedPtr<Provision> provision;
      std::vector<Tag> tags;
      provision.allocate();
      tags.push_back(Tag::DEFAULT_TAG);
      current->clone(&provision->file);
      registerProvider(provision.get(), tags);
      rootProvisions.adoptBack(&provision);
    }
  }
}

void Driver::rescanForNewFactory(ActionFactory* factory) {
  // Apply triggers.
  std::vector<Tag> triggerTags;
  factory->enumerateTriggerTags(std::back_inserter(triggerTags));
  for (unsigned int i = 0; i < triggerTags.size(); i++) {
    for (TagTable::SearchIterator<TagTable::TAG> iter(tagTable, triggerTags[i]); iter.next();) {
      Provision* provision = iter.cell<TagTable::PROVISION>();
      OwnedPtr<Action> action;
      if (factory->tryMakeAction(triggerTags[i], provision->file.get(), &action)) {
        queueNewAction(&action, provision);
      }
    }
  }
}

void Driver::queueNewAction(OwnedPtr<Action>* actionToAdopt, Provision* provision) {
  OwnedPtr<Dashboard::Task> task;
  dashboard->beginTask((*actionToAdopt)->getVerb(), provision->file->canonicalName(),
                       (*actionToAdopt)->isSilent() ? Dashboard::SILENT : Dashboard::NORMAL,
                       &task);

  OwnedPtr<ActionDriver> actionDriver;
  actionDriver.allocate(this, actionToAdopt, provision->file.get(), provision->contentHash, &task);
  actionsByTrigger.insert(std::make_pair(provision, actionDriver.get()));

  // Put new action on front of queue because it was probably triggered by another action that
  // just completed, and it's good to run related actions together to improve cache locality.
  pendingActions.adoptFront(&actionDriver);
}

void Driver::registerProvider(Provision* provision, const std::vector<Tag>& tags) {
  provision->contentHash = provision->file->contentHash();

  for (std::vector<Tag>::const_iterator iter = tags.begin(); iter != tags.end(); ++iter) {
    const Tag& tag = *iter;
    tagTable.add(tag, provision);

    resetDependentActions(tag);

    fireTriggers(tag, provision);
  }
}

void Driver::resetDependentActions(const Tag& tag) {
  std::tr1::unordered_set<Provision*> provisionsToReset;

  std::vector<ActionDriver*> actionsToReset;

  for (DependencyTable::SearchIterator<DependencyTable::TAG> iter(dependencyTable, tag);
       iter.next();) {
    ActionDriver* action = iter.cell<DependencyTable::ACTION>();
    Provision* previousProvider = iter.cell<DependencyTable::PROVISION>();

    if (action->choosePreferredProvider(tag) != previousProvider) {
      // We can't just call reset() here because it could invalidate our iterator.
      actionsToReset.push_back(action);
    }
  }

  for (size_t i = 0; i < actionsToReset.size(); i++) {
    actionsToReset[i]->reset();
  }
}

void Driver::fireTriggers(const Tag& tag, Provision* provision) {
  std::pair<TriggerMap::const_iterator, TriggerMap::const_iterator>
      range = triggers.equal_range(tag);
  for (TriggerMap::const_iterator iter = range.first; iter != range.second; ++iter) {
    OwnedPtr<Action> triggeredAction;
    if (iter->second->tryMakeAction(tag, provision->file.get(), &triggeredAction)) {
      queueNewAction(&triggeredAction, provision);
    }
  }
}

}  // namespace ekam
