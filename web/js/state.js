// Shared UI state.  Classic scripts keep their public event-handler functions,
// while mutable file-view data has one explicit owner.
window.SmartNASState = {
    files: [],
    folders: [],
    currentDirectory: '/',
    showingTrash: false,
    missingSummaryScanStarted: false,
    runtimeConfig: {
        uploadChunkSize: 8 * 1024 * 1024,
        uploadConcurrency: 4,
        webCryptoLimit: 256 * 1024 * 1024
    }
};
