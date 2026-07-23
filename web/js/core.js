        // --- 模态框逻辑 ---
        function openModal(id) {
            document.getElementById(id).classList.add('active');
        }
        function closeModal(id) {
            document.getElementById(id).classList.remove('active');
            // 清空输入框
            const inputs = document.getElementById(id).querySelectorAll('input');
            inputs.forEach(input => input.value = '');
        }

        const APP_PROTOCOL = window.location.protocol === 'https:' ? 'https:' : 'http:';
        const APP_HOST = window.location.hostname || 'localhost';
        const API_BASE = window.location.origin;
        let AGENT_BASE = `${APP_PROTOCOL}//${APP_HOST}:8081`;
        const runtimeConfig = window.SmartNASState.runtimeConfig;

        async function loadRuntimeConfig() {
            try {
                const response = await fetch(`${API_BASE}/api/config`);
                if (!response.ok) return;
                const config = await response.json();
                AGENT_BASE = `${APP_PROTOCOL}//${APP_HOST}:${config.agentPort || 8081}`;
                runtimeConfig.uploadChunkSize = config.uploadChunkSize || runtimeConfig.uploadChunkSize;
                runtimeConfig.uploadConcurrency = config.uploadConcurrency || runtimeConfig.uploadConcurrency;
                runtimeConfig.webCryptoLimit = config.webCryptoLimit || runtimeConfig.webCryptoLimit;
            } catch (error) {
                console.warn('运行时配置加载失败，使用默认值', error);
            }
        }

        function showToast(msg, isError = false) {
            const toast = document.getElementById('toast');
            document.getElementById('toastMsg').innerText = msg;
            toast.style.backgroundColor = isError ? '#ef4444' : '#374151';
            toast.classList.remove('translate-y-20', 'opacity-0');
            setTimeout(() => {
                toast.classList.add('translate-y-20', 'opacity-0');
            }, 3000);
        }

        // --- 核心逻辑：UI 更新 ---
        let summaryTasksByHash = {};
        let summaryPollTimer = null;
        let pendingAskFileHash = '';
        let pendingMoveFileHash = '';
        let pendingMoveFolderPath = '';
        let pendingRename = null;

        function checkAuth() {
            const user = localStorage.getItem('username');
            const token = getAuthToken();
            const userInfoDiv = document.getElementById('user-info');
            updateChatAuthState(Boolean(user && token));

            if (user && token) {
                userInfoDiv.innerHTML = `
                    <div class="flex items-center gap-3">
                        <div class="w-10 h-10 bg-gradient-to-br from-slate-800 to-slate-500 rounded-full flex items-center justify-center text-white font-bold shadow-sm">
                            ${user[0].toUpperCase()}
                        </div>
                        <span class="hidden md:inline text-gray-800 font-bold">${user}</span>
                        <button onclick="logout()" class="icon-btn" title="注销"><i class="fas fa-chevron-down"></i></button>
                    </div>
                `;
                fetchFileList(); // 登录状态下加载文件
                loadSummaryTasks();
                ensureMissingSummaries();
                ensureSearchIndex();
            } else {
                userInfoDiv.innerHTML = `
                    <button onclick="openModal('loginModal')" class="ghost-btn h-10 px-4 text-sm font-bold">登录</button>
                    <button onclick="openModal('registerModal')" class="primary-btn h-10 px-4 text-sm font-bold">注册</button>
                `;
                document.getElementById('file-list').innerHTML = '<div class="p-8 text-center text-gray-400">请先登录以查看文件</div>';
            }
        }

        function logout() {
            localStorage.removeItem('username');
            localStorage.removeItem('token');
            localStorage.removeItem('smartnas_token');
            location.reload(); // 刷新页面恢复初始状态
        }

        function getAuthToken() {
            const token = localStorage.getItem('token') || localStorage.getItem('smartnas_token');
            if (token && !localStorage.getItem('token')) {
                localStorage.setItem('token', token);
            }
            return token;
        }

        function updateChatAuthState(isAuthed) {
            const input = document.getElementById('chatInput');
            const sendButton = document.getElementById('chatSendBtn');
            if (!input || !sendButton) return;

            input.disabled = !isAuthed;
            input.placeholder = isAuthed ? '输入问题...' : '请先登录后使用 AI 助手';
            sendButton.disabled = !isAuthed;
            sendButton.classList.toggle('cursor-not-allowed', !isAuthed);
            sendButton.classList.toggle('opacity-50', !isAuthed);
        }

        // --- 核心逻辑：获取文件列表 ---
        async function submitLogin() {
            const user = document.getElementById('loginUser').value.trim();
            const pwd = document.getElementById('loginPwd').value.trim();
            if (!user || !pwd) {
                showToast("用户名和密码不能为空", true);
                return;
            }
            try {
                const response = await fetch(`${API_BASE}/login`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ user, password: pwd })
                });
                if (response.ok) {
                    localStorage.setItem('username', user); // 核心：保存登录态
                    const data = await response.json();
                    localStorage.setItem('token', data.token); // 保存 JWT token
                    closeModal('loginModal');
                    showToast("登录成功");
                    checkAuth(); // 立即更新 UI
                } else {
                    showToast("登录失败，请检查用户名或密码", true);
                }
            } catch (e) {
                console.error(e);
                showToast("网络请求失败", true);
            }
        }

        async function submitRegister() {
            const user = document.getElementById('regUser').value.trim();
            const pwd = document.getElementById('regPwd').value.trim();
            if (!user || !pwd) {
                showToast("用户名和密码不能为空", true);
                return;
            }
            try {
                const response = await fetch(`${API_BASE}/register`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ user, password: pwd })
                });
                const msg = await response.text();
                closeModal('registerModal');
                showToast(response.ok ? "注册成功，请重新登录" : msg, !response.ok);
            } catch (e) {
                console.error(e);
                showToast("网络请求失败", true);
            }
        }
