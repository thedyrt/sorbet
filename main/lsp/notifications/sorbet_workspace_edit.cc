#include "main/lsp/notifications/sorbet_workspace_edit.h"
#include "core/lsp/TypecheckEpochManager.h"
#include "main/lsp/LSPFileUpdates.h"
#include "main/lsp/LSPIndexer.h"
#include "main/lsp/json_types.h"

using namespace std;

namespace sorbet::realmain::lsp {
SorbetWorkspaceEditTask::SorbetWorkspaceEditTask(const LSPConfiguration &config,
                                                 unique_ptr<SorbetWorkspaceEditParams> params)
    : LSPDangerousTypecheckerTask(config, LSPMethod::SorbetWorkspaceEdit),
      latencyCancelSlowPath(make_unique<Timer>(*config.logger, "latency.cancel_slow_path")), params(move(params)) {
    if (this->params->updates.empty()) {
        latencyCancelSlowPath->cancel();
    }
};

SorbetWorkspaceEditTask::~SorbetWorkspaceEditTask() = default;

LSPTask::Phase SorbetWorkspaceEditTask::finalPhase() const {
    if (params->updates.empty()) {
        // Early-dispatch no-op edits. These can happen if the user opens or changes a file that is not within the
        // current workspace.
        return LSPTask::Phase::PREPROCESS;
    } else {
        return LSPTask::Phase::RUN;
    }
}

void SorbetWorkspaceEditTask::mergeNewer(SorbetWorkspaceEditTask &task) {
    // Merging is only supported *before* we index this update.
    ENFORCE(updates == nullptr && task.updates == nullptr);
    params->merge(*task.params);
    // Don't report a latency metric for merged edits.
    if (task.latencyTimer) {
        task.latencyTimer->cancel();
    }
    if (task.latencyCancelSlowPath) {
        task.latencyCancelSlowPath->cancel();
    }

    // This cached information is now invalid.
    task.cachedFastPathDecisionValid = false;
    task.cachedFastPathDecision = false;
    cachedFastPathDecisionValid = false;
    cachedFastPathDecision = false;
}

void SorbetWorkspaceEditTask::preprocess(LSPPreprocessor &preprocessor) {
    // latencyTimer is assigned prior to preprocess.
    if (this->latencyTimer != nullptr && !params->updates.empty()) {
        params->diagnosticLatencyTimers.push_back(
            make_unique<Timer>(this->latencyTimer->clone("last_diagnostic_latency")));
    }
}

void SorbetWorkspaceEditTask::index(LSPIndexer &indexer) {
    updates = make_unique<LSPFileUpdates>(indexer.commitEdit(*params));
}

void SorbetWorkspaceEditTask::run(LSPTypecheckerDelegate &typechecker) {
    if (latencyTimer != nullptr) {
        latencyTimer->setTag("path", "fast");
    }
    ENFORCE(updates != nullptr);
    if (!updates->canceledSlowPath) {
        latencyCancelSlowPath->cancel();
    }
    // Trigger destructor of Timer, which reports metric.
    latencyCancelSlowPath = nullptr;
    // For consistency; I don't expect this notification to be used for fast path edits.
    startedNotification.Notify();
    if (!updates->canTakeFastPath) {
        Exception::raise("Attempted to run a slow path update on the fast path!");
    }
    const auto newEditCount = updates->editCount - updates->committedEditCount;
    typechecker.typecheckOnFastPath(move(*updates), move(params->diagnosticLatencyTimers));
    if (latencyTimer != nullptr) {
        ENFORCE(newEditCount == params->diagnosticLatencyTimers.size());
        // TODO: Move into pushDiagnostics once we have fast feedback.
        params->diagnosticLatencyTimers.clear();
    }
    prodCategoryCounterAdd("lsp.messages.processed", "sorbet.mergedEdits", newEditCount - 1);
}

void SorbetWorkspaceEditTask::runSpecial(LSPTypechecker &typechecker, WorkerPool &workers) {
    if (latencyTimer != nullptr) {
        latencyTimer->setTag("path", "slow");
    }
    if (!updates->canceledSlowPath) {
        latencyCancelSlowPath->cancel();
    }
    // Trigger destructor of Timer, which reports metric.
    latencyCancelSlowPath = nullptr;
    // Inform the epoch manager that we're going to perform a cancelable typecheck, then notify the
    // processing thread that it's safe to move on.
    typechecker.state().epochManager->startCommitEpoch(updates->epoch);
    startedNotification.Notify();
    const auto newEditCount = updates->editCount - updates->committedEditCount;
    // Only report stats if the edit was committed.
    if (typechecker.typecheck(move(*updates), workers, move(params->diagnosticLatencyTimers))) {
        if (latencyTimer != nullptr) {
            ENFORCE(newEditCount == params->diagnosticLatencyTimers.size());
            // TODO: Move into pushDiagnostics once we have fast feedback.
            params->diagnosticLatencyTimers.clear();
        }
        prodCategoryCounterAdd("lsp.messages.processed", "sorbet.mergedEdits", newEditCount - 1);
    } else if (latencyTimer != nullptr) {
        // Don't report a latency value for canceled slow paths.
        latencyTimer->cancel();
        for (auto &timer : params->diagnosticLatencyTimers) {
            timer->cancel();
        }
        params->diagnosticLatencyTimers.clear();
    }
}

void SorbetWorkspaceEditTask::schedulerWaitUntilReady() {
    startedNotification.WaitForNotification();
}

bool SorbetWorkspaceEditTask::canTakeFastPath(const LSPIndexer &index) const {
    if (updates != nullptr) {
        return updates->canTakeFastPath;
    }
    if (!cachedFastPathDecisionValid) {
        index.computeFileHashes(params->updates);
        cachedFastPathDecision = index.canTakeFastPath(params->updates);
        cachedFastPathDecisionValid = true;
    }
    return cachedFastPathDecision;
}

bool SorbetWorkspaceEditTask::canPreempt(const LSPIndexer &index) const {
    return canTakeFastPath(index);
}

bool SorbetWorkspaceEditTask::needsMultithreading(const LSPIndexer &index) const {
    return !canTakeFastPath(index);
}

const SorbetWorkspaceEditParams &SorbetWorkspaceEditTask::getParams() const {
    return *params;
}

} // namespace sorbet::realmain::lsp