TEMPLATE = subdirs
SUBDIRS = \
        AccountsHelperTest \
        ClientPluginRunnerTest \
        ClientThreadTest \
        PluginRunnerTest \
        ServerActivatorTest \
        ServerPluginRunnerTest \
        ServerThreadTest \
        StorageBookerTest \
        SyncBackupTest \
        SyncQueueTest \
        SyncSessionTest \
        SyncSigHandlerTest \
        SynchronizerTest \
        TransportTrackerTest \

!contains(DEFINES, USE_KEEPALIVE):contains(DEFINES, USE_IPHB) {
SUBDIRS += \
        IPHeartBeatTest \
        SyncSchedulerTest \
}
