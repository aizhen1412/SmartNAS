        // 页面加载时初始化 UI
        window.onload = async () => {
            await loadRuntimeConfig();
            checkAuth();
            updateSortIcons();
        };
