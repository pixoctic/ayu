package com.radolyn.ayugram.proprietary;

public class AyuHistoryHook {
    public static class MinMaxIds {
        public int first;
        public int second;
        public MinMaxIds(int first, int second) {
            this.first = first;
            this.second = second;
        }
    }

    public static MinMaxIds getMinAndMaxIds(Object messArr) {
        // Return dummy values
        return new MinMaxIds(0, 0);
    }

    public static void doHook(int currentAccount, Object messArr, Object messagesDict, 
                              int startId, int endId, long dialogId, int limit, int topicId, 
                              boolean isSecretChat) {
        // Do nothing
    }
}
