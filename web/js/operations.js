        function downloadByHash(hash) {
            const token = getAuthToken();
            if (!token) { showToast("请先登录", true); return; }
            window.open(`${API_BASE}/download?hash=${hash}&token=${token}`);
        }

        function previewByHash(hash) {
            const token = getAuthToken();
            if (!token) { showToast("请先登录", true); return; }
            window.open(`${API_BASE}/api/preview?hash=${hash}&token=${token}`, "_blank");
        }

        async function startSummarizeByHash(hash, options = {}) {
            const token = getAuthToken();
            const force = Boolean(options.force);
            const silent = Boolean(options.silent);
            if (!token) { if (!silent) showToast("请先登录", true); return; }

            if (!silent) showToast("摘要和标签任务已加入队列");
            try {
                const response = await fetch(`${AGENT_BASE}/api/agent/summarize/start`, {
                    method: 'POST',
                    keepalive: true,
                    headers: {
                        'Content-Type': 'application/json',
                        'Authorization': `Bearer ${token}`
                    },
                    body: JSON.stringify({ hash, force })
                });
                const data = await response.json();
                if (!response.ok) {
                    throw new Error(data.detail || data.message || `AI 服务返回 ${response.status}`);
                }

                summaryTasksByHash[hash] = data;
                updateStats();
                applyFilterAndSort();
                ensureSummaryPolling();
            } catch (e) {
                console.error("摘要生成失败:", e);
                if (!silent) showToast(`摘要生成失败: ${e.message}`, true);
            }
        }

        async function loadSummaryTasks() {
            const token = getAuthToken();
            if (!token) return;
            try {
                const response = await fetch(`${AGENT_BASE}/api/agent/summarize/tasks`, {
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                if (!response.ok) return;
                const data = await response.json();
                (data.tasks || []).forEach(task => {
                    summaryTasksByHash[task.hash] = task;
                });
                updateStats();
                applyFilterAndSort();
                if (Object.values(summaryTasksByHash).some(task => ['pending', 'running', 'cancel_requested'].includes(task.status))) {
                    ensureSummaryPolling();
                }
            } catch (error) {
                console.error('加载摘要任务失败:', error);
            }
        }

        function ensureSummaryPolling() {
            if (summaryPollTimer) return;
            summaryPollTimer = setInterval(pollSummaryTasks, 1800);
            pollSummaryTasks();
        }

        async function pollSummaryTasks() {
            const token = getAuthToken();
            if (!token) return;

            const activeTasks = Object.values(summaryTasksByHash).filter(t => ['pending', 'running', 'cancel_requested'].includes(t.status));
            if (activeTasks.length === 0) {
                clearInterval(summaryPollTimer);
                summaryPollTimer = null;
                updateStats();
                return;
            }

            let shouldRefresh = false;
            await Promise.all(activeTasks.map(async task => {
                try {
                    const response = await fetch(`${AGENT_BASE}/api/agent/summarize/status/${task.id}`, {
                        headers: { 'Authorization': `Bearer ${token}` }
                    });
                    const data = await response.json();
                    if (response.ok) {
                        summaryTasksByHash[data.hash] = data;
                        if (['success', 'failed', 'cancelled'].includes(data.status)) {
                            shouldRefresh = true;
                            if (data.status === 'success') showToast("摘要和标签已生成");
                            if (data.status === 'failed') showToast(`摘要和标签失败: ${data.message}`, true);
                        }
                    }
                } catch (e) {
                    console.error("摘要状态轮询失败:", e);
                }
            }));

            if (shouldRefresh) fetchFileList();
            updateStats();
            applyFilterAndSort();
        }

        async function cancelSummaryTask(taskId) {
            const token = getAuthToken();
            if (!token) return;
            const response = await fetch(`${AGENT_BASE}/api/agent/summarize/cancel/${taskId}`, {
                method: 'POST', headers: { 'Authorization': `Bearer ${token}` }
            });
            const data = await response.json();
            if (!response.ok) return showToast(data.detail || '取消任务失败', true);
            summaryTasksByHash[data.hash] = data;
            showToast('正在取消任务');
            ensureSummaryPolling();
            applyFilterAndSort();
        }

        async function retrySummaryTask(taskId) {
            const token = getAuthToken();
            if (!token) return;
            const response = await fetch(`${AGENT_BASE}/api/agent/summarize/retry/${taskId}`, {
                method: 'POST', headers: { 'Authorization': `Bearer ${token}` }
            });
            const data = await response.json();
            if (!response.ok) return showToast(data.detail || '重试任务失败', true);
            summaryTasksByHash[data.hash] = data;
            showToast('任务已重新加入队列');
            ensureSummaryPolling();
            applyFilterAndSort();
        }

        let fileToDelete = null;
        let folderToDelete = null;
        function confirmDelete(hash) {
            fileToDelete = hash;
            folderToDelete = null;
            document.getElementById('deleteModalTitle').textContent = '删除文件';
            document.getElementById('deleteModalDescription').textContent = '文件会移入回收站，之后可以恢复或永久删除。';
            openModal('deleteModal');
            document.getElementById('confirmDeleteBtn').onclick = async () => {
                await deleteByHash(fileToDelete);
            };
        }

        function confirmFolderDelete(encodedPath) {
            folderToDelete = decodeURIComponent(encodedPath);
            fileToDelete = null;
            document.getElementById('deleteModalTitle').textContent = '删除文件夹';
            document.getElementById('deleteModalDescription').textContent = '仅空文件夹可以删除，此操作不会删除任何文件。';
            openModal('deleteModal');
            document.getElementById('confirmDeleteBtn').onclick = async () => {
                await deleteFolder(folderToDelete);
            };
        }

        async function deleteByHash(hash) {
            const token = getAuthToken();
            if (!token) {
                showToast("请先登录", true);
                closeModal('deleteModal');
                return;
            }

            try {
                const response = await fetch(`${API_BASE}/api/delete?hash=${hash}`, {
                    method: 'POST',
                    headers: { 'Authorization': `Bearer ${token}` }
                });

                if (response.ok) {
                    showToast("删除成功");
                    fetchFileList(); // 刷新列表
                } else {
                    showToast("删除失败或没有权限", true);
                }
            } catch (e) {
                console.error("删除出错:", e);
                showToast("删除异常", true);
            } finally {
                closeModal('deleteModal');
                fileToDelete = null;
            }
        }

        async function restoreByHash(hash) {
            const token = getAuthToken();
            if (!token) return;
            const res = await fetch(`${API_BASE}/api/restore?hash=${encodeURIComponent(hash)}`, {
                method: 'POST',
                headers: { 'Authorization': `Bearer ${token}` }
            });
            showToast(res.ok ? "已恢复" : "恢复失败", !res.ok);
            if (res.ok) {
                startSummarizeByHash(hash, { force: true, silent: true });
            }
            fetchFileList();
        }

        async function purgeByHash(hash) {
            const token = getAuthToken();
            if (!token) return;
            if (!confirm("永久删除后无法恢复，确认继续？")) return;
            const res = await fetch(`${API_BASE}/api/purge?hash=${encodeURIComponent(hash)}`, {
                method: 'POST',
                headers: { 'Authorization': `Bearer ${token}` }
            });
            showToast(res.ok ? "已永久删除" : "永久删除失败", !res.ok);
            fetchFileList();
        }

        function renameByHash(hash, oldName) {
            const token = getAuthToken();
            if (!token) return;
            const decodedOldName = decodeURIComponent(oldName || '').replace(/&quot;/g, '"').replace(/&#39;/g, "'");
            pendingRename = { type: 'file', id: hash };
            document.getElementById('renameModalTitle').textContent = '重命名文件';
            const input = document.getElementById('renameInput');
            input.value = decodedOldName;
            openModal('renameModal');
            setTimeout(() => { input.focus(); input.select(); }, 0);
        }

        function renameFolder(encodedPath, encodedName) {
            pendingRename = { type: 'folder', id: decodeURIComponent(encodedPath) };
            document.getElementById('renameModalTitle').textContent = '重命名文件夹';
            const input = document.getElementById('renameInput');
            input.value = decodeURIComponent(encodedName);
            openModal('renameModal');
            setTimeout(() => { input.focus(); input.select(); }, 0);
        }

        async function submitRename() {
            const token = getAuthToken();
            const name = document.getElementById('renameInput').value.trim();
            if (!token || !pendingRename || !name) return;
            const button = document.getElementById('renameSubmitBtn');
            button.disabled = true;
            button.classList.add('opacity-60');
            const target = pendingRename;
            const url = target.type === 'file'
                ? `${API_BASE}/api/rename?hash=${encodeURIComponent(target.id)}&name=${encodeURIComponent(name)}`
                : `${API_BASE}/api/folders/rename?path=${encodeURIComponent(target.id)}&name=${encodeURIComponent(name)}`;
            try {
                const res = await fetch(url, {
                    method: 'POST',
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                showToast(res.ok ? "重命名成功" : "重命名失败", !res.ok);
                if (res.ok) {
                    closeModal('renameModal');
                    fetchFileList();
                    if (target.type === 'file') {
                        startSummarizeByHash(target.id, { force: true, silent: true });
                    }
                }
            } catch (error) {
                showToast(`重命名失败：${error.message}`, true);
            } finally {
                button.disabled = false;
                button.classList.remove('opacity-60');
            }
        }

        async function loadMovePathSuggestions(token) {
            let folderPaths = globalFolders.map(folder => folder.path);
            try {
                const response = await fetch(`${API_BASE}/api/folders`, {
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                if (response.ok) {
                    const folders = await response.json();
                    folderPaths = folders.map(folder => folder.path);
                }
            } catch (_) {}
            const paths = ['/', currentDir, ...folderPaths]
                .filter(Boolean)
                .filter((path, index, values) => values.indexOf(path) === index);
            document.getElementById('movePathSuggestions').innerHTML = paths.map(path => `
                <button type="button" data-path="${escapeHtml(path)}" onclick="selectMovePath(this.dataset.path)"
                    class="px-3 py-1.5 rounded-lg border border-slate-200 bg-white hover:border-blue-300 hover:text-blue-600 text-xs font-semibold text-slate-600">
                    <i class="far fa-folder mr-1.5"></i>${escapeHtml(path)}
                </button>
            `).join('');
        }

        async function moveByHash(hash) {
            const token = getAuthToken();
            if (!token) return;
            pendingMoveFileHash = hash;
            pendingMoveFolderPath = '';
            const input = document.getElementById('moveFilePath');
            input.value = currentDir || '/';
            await loadMovePathSuggestions(token);
            openModal('moveFileModal');
            setTimeout(() => input.focus(), 0);
        }

        async function moveFolderByPath(encodedPath) {
            const token = getAuthToken();
            if (!token) return;
            pendingMoveFileHash = '';
            pendingMoveFolderPath = decodeURIComponent(encodedPath);
            const input = document.getElementById('moveFilePath');
            input.value = currentDir || '/';
            await loadMovePathSuggestions(token);
            openModal('moveFileModal');
            setTimeout(() => input.focus(), 0);
        }

        function selectMovePath(path) {
            document.getElementById('moveFilePath').value = path || '/';
        }

        async function submitMoveFile() {
            const token = getAuthToken();
            const dir = document.getElementById('moveFilePath').value.trim();
            if (!dir) return;
            const button = document.getElementById('moveFileSubmitBtn');
            button.disabled = true;
            button.classList.add('opacity-60');
            try {
                const movingFolder = Boolean(pendingMoveFolderPath);
                const url = movingFolder
                    ? `${API_BASE}/api/folders/move?path=${encodeURIComponent(pendingMoveFolderPath)}&dir=${encodeURIComponent(dir)}`
                    : `${API_BASE}/api/move?hash=${encodeURIComponent(pendingMoveFileHash)}&dir=${encodeURIComponent(dir)}`;
                const res = await fetch(url, {
                    method: 'POST',
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                showToast(res.ok ? "移动成功" : "移动失败", !res.ok);
                if (res.ok) {
                    closeModal('moveFileModal');
                    fetchFileList();
                    if (!movingFolder) {
                        startSummarizeByHash(pendingMoveFileHash, { force: true, silent: true });
                    }
                }
            } catch (error) {
                showToast(`移动失败：${error.message}`, true);
            } finally {
                button.disabled = false;
                button.classList.remove('opacity-60');
            }
        }

        async function deleteFolder(path) {
            const token = getAuthToken();
            if (!token || !path) return;
            try {
                const res = await fetch(`${API_BASE}/api/folders/delete?path=${encodeURIComponent(path)}`, {
                    method: 'POST',
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                if (res.ok) {
                    showToast('文件夹已删除');
                    fetchFileList();
                } else {
                    const data = await res.json().catch(() => ({}));
                    showToast(data.error === 'Folder is not empty or does not exist' ? '只能删除空文件夹' : '删除文件夹失败', true);
                }
            } catch (error) {
                showToast(`删除文件夹失败：${error.message}`, true);
            } finally {
                closeModal('deleteModal');
                folderToDelete = null;
            }
        }

        async function shareByHash(hash) {
            const token = getAuthToken();
            if (!token) return;
            const res = await fetch(`${API_BASE}/api/share/create?hash=${encodeURIComponent(hash)}&hours=24`, {
                method: 'POST',
                headers: { 'Authorization': `Bearer ${token}` }
            });
            const data = await res.json();
            if (!res.ok) {
                showToast("分享失败", true);
                return;
            }
            const url = `${window.location.origin}${data.url}`;
            try {
                await navigator.clipboard.writeText(url);
                showToast("分享链接已复制，有效期 24 小时");
            } catch {
                showToast(url);
            }
        }

        async function createFolder() {
            const token = getAuthToken();
            if (!token) return;
            const name = prompt("文件夹名称");
            if (!name) return;
            const path = currentDir === "/" ? `/${name}` : `${currentDir}/${name}`;
            const res = await fetch(`${API_BASE}/api/folders?path=${encodeURIComponent(path)}`, {
                method: 'POST',
                headers: { 'Authorization': `Bearer ${token}` }
            });
            showToast(res.ok ? "文件夹已创建" : "创建文件夹失败", !res.ok);
            fetchFileList();
        }
