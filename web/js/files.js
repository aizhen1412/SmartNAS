        async function fetchFileList() {
            const token = getAuthToken(); // 拿 JWT token
            if (!token) return;

            try {
                const response = await fetch(listUrl(), {
                    method: 'GET',
                    headers: { 'Authorization': `Bearer ${token}` }
                });

                if (!response.ok) throw new Error("服务器拒绝访问");

                const payload = await response.json();
                window.SmartNASState.files = Array.isArray(payload) ? payload : (payload.files || []);
                window.SmartNASState.folders = Array.isArray(payload) ? [] : (payload.folders || []);
                updateStats();
                renderBreadcrumb();
                applyFilterAndSort();
            } catch (e) {
                console.error("获取失败:", e);
                document.getElementById('file-list').innerHTML = '<div class="p-8 text-center text-red-400">列表加载失败，请检查登录状态</div>';
            }
        }

        let currentSortField = 'time'; // 默认按时间
        let currentSortDesc = true;    // 默认降序

        function toggleSort(field) {
            if (currentSortField === field) {
                currentSortDesc = !currentSortDesc; // 切换升降序
            } else {
                currentSortField = field;
                currentSortDesc = false; // 新字段默认升序
            }
            updateSortIcons();
            applyFilterAndSort();
        }

        function updateSortIcons() {
            // 重置所有图标
            const nameIcon = document.getElementById('sortIcon_name');
            const sizeIcon = document.getElementById('sortIcon_size');
            const timeIcon = document.getElementById('sortIcon_time');
            if (nameIcon) nameIcon.className = "fas fa-sort ml-2 text-gray-300";
            if (sizeIcon) sizeIcon.className = "fas fa-sort ml-2 text-gray-300";
            if (timeIcon) timeIcon.className = "fas fa-sort ml-2 text-gray-300";

            const activeIcon = document.getElementById(`sortIcon_${currentSortField}`);
            if (activeIcon) {
                activeIcon.className = currentSortDesc ? "fas fa-sort-down ml-2 text-blue-600" : "fas fa-sort-up ml-2 text-blue-600";
            }
        }

        function applyFilterAndSort() {
            const query = (document.getElementById('searchInput').value || '').toLowerCase().trim();

            let filtered = window.SmartNASState.files.filter(f => {
                const summary = (f.summary || '').toLowerCase();
                const tags = Array.isArray(f.tags) ? f.tags.join(' ').toLowerCase() : '';
                return f.name.toLowerCase().includes(query) || summary.includes(query) || tags.includes(query);
            });

            filtered.sort((a, b) => {
                let cmp = 0;
                if (currentSortField === 'name') {
                    cmp = a.name.localeCompare(b.name);
                } else if (currentSortField === 'size') {
                    cmp = a.rawSize - b.rawSize;
                } else {
                    cmp = a.uploadTime - b.uploadTime;
                }
                return currentSortDesc ? -cmp : cmp;
            });

            renderFiles(filtered);
        }

        function updateStats() {
            const activeTasks = Object.values(summaryTasksByHash).filter(t => ['pending', 'running', 'cancel_requested'].includes(t.status)).length;
            const summarized = window.SmartNASState.files.filter(f => (f.summary || '').trim()).length;
            const statFiles = document.getElementById('statFiles');
            const statSummaries = document.getElementById('statSummaries');
            const statTasks = document.getElementById('statTasks');
            if (statFiles) statFiles.innerText = window.SmartNASState.files.length;
            if (statSummaries) statSummaries.innerText = summarized;
            if (statTasks) statTasks.innerText = activeTasks;
        }

        function listUrl() {
            const params = new URLSearchParams();
            params.set('dir', window.SmartNASState.currentDirectory);
            if (window.SmartNASState.showingTrash) params.set('deleted', '1');
            return `${API_BASE}/api/list?${params.toString()}`;
        }

        function renderBreadcrumb() {
            const el = document.getElementById('breadcrumb');
            const filesViewBtn = document.getElementById('filesViewBtn');
            const trashViewBtn = document.getElementById('trashViewBtn');
            if (!el) return;
            if (filesViewBtn) filesViewBtn.classList.toggle('active', !window.SmartNASState.showingTrash);
            if (trashViewBtn) trashViewBtn.classList.toggle('active', window.SmartNASState.showingTrash);

            if (window.SmartNASState.showingTrash) {
                el.innerHTML = '<span class="font-semibold text-red-500"><i class="fas fa-trash-can mr-2"></i>回收站</span>';
                return;
            }

            const parts = window.SmartNASState.currentDirectory === '/' ? [] : window.SmartNASState.currentDirectory.split('/').filter(Boolean);
            let path = '';
            const crumbs = [`<button onclick="openDirectory('/')" class="hover:text-blue-600 font-semibold">根目录</button>`];
            parts.forEach(part => {
                path += '/' + part;
                crumbs.push(`<button onclick="openDirectory('${escapeHtml(path)}')" class="hover:text-blue-600">${escapeHtml(part)}</button>`);
            });
            el.innerHTML = crumbs.join('<span class="mx-2 text-gray-300">/</span>');
        }

        function openDirectory(path) {
            window.SmartNASState.currentDirectory = path || '/';
            window.SmartNASState.showingTrash = false;
            fetchFileList();
        }

        function toggleTrash() {
            window.SmartNASState.showingTrash = !window.SmartNASState.showingTrash;
            fetchFileList();
        }

        function setTrashView(value) {
            window.SmartNASState.showingTrash = value;
            fetchFileList();
        }

        // 格式化时间戳显示
        function formatUploadTime(timestamp) {
            if (!timestamp) return '-';
            // C++ time_since_epoch().count() 可能是纳秒或微秒级别
            let ms = timestamp;
            if (timestamp > 1e15) ms = Math.floor(timestamp / 1000000); // ns to ms
            else if (timestamp > 1e12 && timestamp < 1e15) ms = Math.floor(timestamp / 1000); // us to ms
            else if (timestamp < 1e11) ms = timestamp * 1000; // s to ms

            const d = new Date(ms);
            return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')} ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
        }

        function escapeHtml(value) {
            return String(value || '')
                .replace(/&/g, '&amp;')
                .replace(/</g, '&lt;')
                .replace(/>/g, '&gt;')
                .replace(/"/g, '&quot;')
                .replace(/'/g, '&#39;');
        }

        function cleanModelText(value) {
            return String(value || '').replace(/\*/g, '');
        }

        function getFileIcon(name) {
            const ext = (name.split('.').pop() || '').toLowerCase();
            if (['pdf'].includes(ext)) return 'fa-file-pdf text-red-500';
            if (['docx', 'doc'].includes(ext)) return 'fa-file-word text-blue-600';
            if (['xlsx', 'xls', 'csv'].includes(ext)) return 'fa-file-excel text-emerald-600';
            if (['pptx', 'ppt'].includes(ext)) return 'fa-file-powerpoint text-orange-500';
            if (['html', 'htm', 'json', 'txt', 'md', 'xml', 'svg', 'ipynb'].includes(ext)) return 'fa-file-lines text-slate-500';
            if (['png', 'jpg', 'jpeg', 'webp', 'gif', 'bmp', 'tiff', 'tif'].includes(ext)) return 'fa-file-image text-cyan-600';
            if (['mp4'].includes(ext)) return 'fa-file-video text-violet-600';
            if (['mp3', 'wav', 'm4a'].includes(ext)) return 'fa-file-audio text-fuchsia-600';
            if (['zip', 'epub'].includes(ext)) return 'fa-file-zipper text-amber-600';
            return 'fa-file text-slate-500';
        }

        function getFileTypeLabel(name) {
            const ext = (name.split('.').pop() || '').toLowerCase();
            if (!ext || ext === name.toLowerCase()) return '文件';
            const labels = {
                pdf: 'PDF',
                doc: 'DOC',
                docx: 'DOCX',
                xls: 'XLS',
                xlsx: 'XLSX',
                csv: 'CSV',
                ppt: 'PPT',
                pptx: 'PPTX',
                md: 'Markdown',
                txt: 'TXT',
                png: 'PNG',
                jpg: 'JPG',
                jpeg: 'JPEG',
                webp: 'WEBP',
                json: 'JSON',
                mp4: 'MP4',
                mp3: 'MP3',
                zip: 'ZIP'
            };
            return labels[ext] || ext.toUpperCase();
        }

        function isSummarizableFile(name) {
            const ext = (name.split('.').pop() || '').toLowerCase();
            return [
                'pdf', 'docx', 'pptx', 'xlsx', 'txt', 'csv', 'json', 'html', 'htm',
                'md', 'xml', 'epub', 'zip', 'ipynb',
                'jpg', 'jpeg', 'png', 'webp', 'gif', 'bmp', 'tiff', 'tif', 'svg',
                'wav', 'mp3', 'm4a', 'mp4'
            ].includes(ext);
        }

        function getSummaryBadge(file) {
            const task = summaryTasksByHash[file.hash];
            if (task && ['pending', 'running', 'cancel_requested'].includes(task.status)) {
                return '<span class="summary-pill summary-running"><i class="fas fa-spinner fa-spin"></i>摘要中</span>';
            }
            if (task && task.status === 'failed') {
                return '<span class="summary-pill summary-failed"><i class="fas fa-circle-exclamation"></i>失败</span>';
            }
            if ((file.summary || '').trim()) {
                return '<span class="summary-pill summary-ready"><i class="fas fa-check"></i>已索引</span>';
            }
            return '<span class="summary-pill summary-empty"><i class="fas fa-minus"></i>未摘要</span>';
        }

        function closeActionMenu() {
            const menu = document.getElementById('actionMenu');
            if (menu) menu.classList.remove('active');
        }

        function openFileMenu(hash, encodedName, canSummarize, event) {
            event.stopPropagation();
            const menu = document.getElementById('actionMenu');
            const name = decodeURIComponent(encodedName || '');
            const disabledClass = canSummarize ? '' : 'opacity:0.45;pointer-events:none;';
            const task = summaryTasksByHash[hash];
            let taskAction = '';
            if (task && ['pending', 'running', 'cancel_requested'].includes(task.status)) {
                taskAction = `<button onclick="cancelSummaryTask('${task.id}'); closeActionMenu();"><i class="fas fa-stop"></i>取消任务</button>`;
            } else if (task && ['failed', 'cancelled'].includes(task.status)) {
                taskAction = `<button onclick="retrySummaryTask('${task.id}'); closeActionMenu();"><i class="fas fa-rotate-right"></i>重试任务</button>`;
            }

            if (window.SmartNASState.showingTrash) {
                menu.innerHTML = `
                    <button onclick="restoreByHash('${hash}'); closeActionMenu();"><i class="fas fa-rotate-left"></i>恢复</button>
                    <button class="danger" onclick="purgeByHash('${hash}'); closeActionMenu();"><i class="fas fa-fire"></i>永久删除</button>
                `;
            } else {
                menu.innerHTML = `
                    ${taskAction}
                    <button onclick="askFileByHash('${hash}'); closeActionMenu();" style="${disabledClass}"><i class="fas fa-circle-question"></i>问这个文件</button>
                    <button onclick="renameByHash('${hash}', '${encodeURIComponent(name)}'); closeActionMenu();"><i class="fas fa-pen"></i>重命名</button>
                    <button onclick="moveByHash('${hash}'); closeActionMenu();"><i class="fas fa-folder-tree"></i>移动</button>
                    <button onclick="shareByHash('${hash}'); closeActionMenu();"><i class="fas fa-link"></i>分享</button>
                    <button class="danger" onclick="confirmDelete('${hash}'); closeActionMenu();"><i class="fas fa-trash"></i>移入回收站</button>
                `;
            }

            const rect = event.currentTarget.getBoundingClientRect();
            menu.style.top = `${Math.max(8, Math.min(rect.bottom + 8, window.innerHeight - 260))}px`;
            menu.style.left = `${Math.max(8, Math.min(rect.left - 145, window.innerWidth - 200))}px`;
            menu.classList.add('active');
        }

        function openFolderMenu(encodedPath, encodedName, event) {
            event.stopPropagation();
            const menu = document.getElementById('actionMenu');
            const path = decodeURIComponent(encodedPath);
            const name = decodeURIComponent(encodedName);
            const safePath = encodeURIComponent(path).replace(/'/g, '%27');
            const safeName = encodeURIComponent(name).replace(/'/g, '%27');
            menu.innerHTML = `
                <button onclick="renameFolder('${safePath}', '${safeName}'); closeActionMenu();"><i class="fas fa-pen"></i>重命名</button>
                <button onclick="moveFolderByPath('${safePath}'); closeActionMenu();"><i class="fas fa-folder-tree"></i>移动</button>
                <button class="danger" onclick="confirmFolderDelete('${safePath}'); closeActionMenu();"><i class="fas fa-trash"></i>删除</button>
            `;
            const rect = event.currentTarget.getBoundingClientRect();
            menu.style.top = `${Math.max(8, Math.min(rect.bottom + 8, window.innerHeight - 150))}px`;
            menu.style.left = `${Math.max(8, Math.min(rect.left - 145, window.innerWidth - 200))}px`;
            menu.classList.add('active');
        }

        document.addEventListener('click', closeActionMenu);
        window.addEventListener('resize', closeActionMenu);

        function renderFiles(files) {
            const listDiv = document.getElementById('file-list');
            if (files.length === 0 && window.SmartNASState.folders.length === 0) {
                listDiv.innerHTML = '<div class="p-8 text-center text-gray-400">暂无文件</div>';
                return;
            }

            const folderHtml = window.SmartNASState.showingTrash ? '' : window.SmartNASState.folders.map(folder => {
                const safeName = escapeHtml(folder.name);
                const safePath = escapeHtml(folder.path);
                const encodedPath = encodeURIComponent(folder.path).replace(/'/g, '%27');
                const encodedFolderName = encodeURIComponent(folder.name).replace(/'/g, '%27');
                return `
                <div class="grid grid-cols-1 md:grid-cols-12 gap-3 md:gap-4 px-5 py-3 items-center file-item transition bg-amber-50/30">
                    <div class="md:col-span-4 flex items-center space-x-3 overflow-hidden">
                        <div class="w-9 h-9 rounded-lg bg-amber-100 flex items-center justify-center flex-shrink-0">
                            <i class="fas fa-folder text-amber-500 text-lg"></i>
                        </div>
                        <button onclick="openDirectory('${safePath}')" class="text-sm font-semibold text-gray-800 truncate hover:text-blue-600 text-left">${safeName}</button>
                    </div>
                    <div class="md:col-span-2 text-sm text-gray-400">文件夹</div>
                    <div class="md:col-span-2"></div>
                    <div class="md:col-span-3 text-sm text-gray-400">${formatUploadTime(folder.createdTime)}</div>
                    <div class="md:col-span-1 flex md:justify-end">
                        <button onclick="openFolderMenu('${encodedPath}', '${encodedFolderName}', event)" class="icon-btn text-slate-600" title="更多">
                            <i class="fas fa-ellipsis"></i>
                        </button>
                    </div>
                </div>`;
            }).join('');

            const fileHtml = files.map(file => {
                const safeName = escapeHtml(file.name);
                const safeHash = escapeHtml(file.hash);
                const encodedName = encodeURIComponent(file.name || '');
                const summary = (file.summary || '').trim();
                const safeSummary = escapeHtml(cleanModelText(summary));
                const tags = Array.isArray(file.tags) ? file.tags : [];
                const tagsHtml = tags.slice(0, 5).map(tag => {
                    const safeTag = escapeHtml(String(tag));
                    return `<span class="file-tag" title="${safeTag}">${safeTag}</span>`;
                }).join('');
                const fileIcon = getFileIcon(file.name);
                const fileType = getFileTypeLabel(file.name);
                const task = summaryTasksByHash[file.hash];
                const taskText = task && task.status === 'failed' ? escapeHtml(task.message || '摘要失败') : '';
                const canSummarize = isSummarizableFile(file.name) && !window.SmartNASState.showingTrash;
                return `
                <div class="grid grid-cols-1 md:grid-cols-12 gap-3 md:gap-4 px-5 py-3 items-center file-item transition">
                    <div class="md:col-span-4 flex items-start space-x-3 overflow-hidden">
                        <div class="w-9 h-9 rounded-lg bg-slate-100 flex items-center justify-center flex-shrink-0">
                            <i class="far ${fileIcon} text-lg"></i>
                        </div>
                        <div class="min-w-0">
                            <div class="flex items-center gap-2 min-w-0">
                                <div class="text-sm font-semibold text-gray-800 truncate" title="${safeName}">${safeName}</div>
                                ${getSummaryBadge(file)}
                            </div>
                            <div class="mt-1 text-xs text-gray-500 summary-text">${taskText || (summary ? safeSummary : (canSummarize ? '等待生成摘要' : '暂不参与摘要'))}</div>
                            ${tagsHtml ? `<div class="file-tags">${tagsHtml}</div>` : ''}
                        </div>
                    </div>
                    <div class="md:col-span-2 text-sm text-gray-500">${fileType}</div>
                    <div class="md:col-span-2 text-sm text-gray-500">${file.size}</div>
                    <div class="md:col-span-3 text-sm text-gray-500">${formatUploadTime(file.uploadTime)}</div>
                    <div class="md:col-span-1 flex md:justify-end space-x-1 flex-shrink-0">
                        ${window.SmartNASState.showingTrash ? `
                            <button onclick="restoreByHash('${safeHash}')" class="icon-btn text-emerald-600" title="恢复"><i class="fas fa-rotate-left"></i></button>
                            <button onclick="openFileMenu('${safeHash}', '${encodedName}', ${canSummarize}, event)" class="icon-btn text-slate-600" title="更多"><i class="fas fa-ellipsis"></i></button>
                        ` : `
                            <button onclick="previewByHash('${safeHash}')" class="icon-btn text-emerald-600" title="预览"><i class="fas fa-eye"></i></button>
                            <button onclick="downloadByHash('${safeHash}')" class="icon-btn text-blue-600" title="下载"><i class="fas fa-download"></i></button>
                            <button onclick="openFileMenu('${safeHash}', '${encodedName}', ${canSummarize}, event)" class="icon-btn text-slate-600" title="更多"><i class="fas fa-ellipsis"></i></button>
                        `}
                    </div>
                </div>
            `;
            }).join('');
            listDiv.innerHTML = folderHtml + fileHtml;
        }

        // --- 之前实现的业务逻辑 ---
