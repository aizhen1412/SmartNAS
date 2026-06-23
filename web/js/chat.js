    let chatAbortController = null;

    function setChatGenerating(generating) {
        const sendButton = document.getElementById('chatSendBtn');
        const stopButton = document.getElementById('chatStopBtn');
        const input = document.getElementById('chatInput');
        if (sendButton) sendButton.classList.toggle('hidden', generating);
        if (stopButton) stopButton.classList.toggle('hidden', !generating);
        if (input) input.disabled = generating;
    }

    function stopChatGeneration() {
        if (chatAbortController) chatAbortController.abort();
    }

    async function sendChatMessage() {
        const input = document.getElementById('chatInput');
        const text = input.value.trim();
        if (!text) return;

        const history = document.getElementById('chatHistory');

        // 追加用户的消息
        const userMsg = document.createElement('div');
        userMsg.className = 'flex items-start justify-end mb-2';
        userMsg.innerHTML = `<span class="bg-gray-100 px-3 py-2 rounded-lg rounded-tr-none inline-block text-right text-gray-800">${escapeHtml(text)}</span><i class="fas fa-user text-gray-400 mt-1 ml-2"></i>`;
        history.appendChild(userMsg);

        input.value = '';
        history.scrollTop = history.scrollHeight;

        // 追加一个 "思考中..." 提示
        const thinkingMsg = document.createElement('div');
        thinkingMsg.className = 'flex items-start mb-2 text-gray-400 text-xs italic thinking-msg';
        thinkingMsg.innerHTML = '<i class="fas fa-robot mt-1 mr-2"></i>思考中...';
        history.appendChild(thinkingMsg);
        history.scrollTop = history.scrollHeight;

        try {
            const token = getAuthToken();
            if (!token) {
                updateChatAuthState(false);
                throw new Error("请先登录后再使用 AI 助手");
            }
            chatAbortController = new AbortController();
            setChatGenerating(true);
            const res = await fetch(`${AGENT_BASE}/api/agent/chat/stream`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Authorization': `Bearer ${token}`
                },
                body: JSON.stringify({ prompt: text }),
                signal: chatAbortController.signal
            });
            if (!res.ok) {
                let detail = `AI 服务返回 ${res.status}`;
                try {
                    const data = await res.json();
                    detail = data.detail || data.message || detail;
                } catch (_) {}
                throw new Error(detail);
            }

            // 移除思考中
            const thinkNodes = history.querySelectorAll('.thinking-msg');
            thinkNodes.forEach(node => node.remove());

            // 准备 AI 回答气泡
            const aiMsg = document.createElement('div');
            aiMsg.className = 'flex items-start mb-2';
            aiMsg.innerHTML = `<i class="fas fa-robot text-blue-500 mt-1 mr-2"></i><span class="bg-blue-50 px-3 py-2 rounded-lg rounded-tl-none inline-block ai-text whitespace-pre-wrap"></span>`;
            history.appendChild(aiMsg);
            const aiTextSpan = aiMsg.querySelector('.ai-text');

            const reader = res.body.getReader();
            const decoder = new TextDecoder();
            let buffer = '';
            let answer = '';
            while (true) {
                const { value, done } = await reader.read();
                buffer += decoder.decode(value || new Uint8Array(), { stream: !done });
                const events = buffer.split('\n\n');
                buffer = events.pop() || '';
                for (const event of events) {
                    const line = event.split('\n').find(item => item.startsWith('data:'));
                    if (!line) continue;
                    const data = JSON.parse(line.slice(5).trim());
                    if (data.type === 'delta') {
                        answer += cleanModelText(data.content);
                        aiTextSpan.textContent = answer;
                        history.scrollTop = history.scrollHeight;
                    } else if (data.type === 'error') {
                        throw new Error(data.message || '流式生成失败');
                    }
                }
                if (done) break;
            }
            history.scrollTop = history.scrollHeight;

        } catch (error) {
            console.error('Chat Error:', error);
            const thinkNodes = history.querySelectorAll('.thinking-msg');
            thinkNodes.forEach(node => node.remove());
            if (error.name === 'AbortError') {
                showToast('已停止生成');
            } else {
                const errorMsg = document.createElement('div');
                errorMsg.className = 'flex items-start mb-2';
                const detail = error instanceof TypeError
                    ? `AI 服务未启动或不可达，请确认 Agent 正在运行：${AGENT_BASE}`
                    : error.message;
                errorMsg.innerHTML = `<i class="fas fa-robot text-red-500 mt-1 mr-2"></i><span class="bg-red-50 text-red-600 px-3 py-2 rounded-lg rounded-tl-none inline-block"></span>`;
                errorMsg.querySelector('span').textContent = `抱歉，无法连接到 AI 引擎: ${detail}`;
                history.appendChild(errorMsg);
            }
        } finally {
            chatAbortController = null;
            setChatGenerating(false);
        }
    }

    function handleChatKeyPress(e) {
        if (e.key === 'Enter') sendChatMessage();
    }

    async function clearChatHistory() {
        const token = getAuthToken();
        if (!token) return;
        try {
            await fetch(`${AGENT_BASE}/api/agent/clear_history`, {
                method: 'POST',
                headers: { 'Authorization': `Bearer ${token}` }
            });
            document.getElementById('chatHistory').innerHTML = `
                <div class="flex items-start mb-2">
                    <i class="fas fa-robot text-blue-500 mt-1 mr-2"></i>
                    <span class="bg-white px-3 py-2 rounded-lg rounded-tl-none inline-block border border-slate-100">对话已清空。</span>
                </div>
            `;
        } catch (e) {
            showToast("清空对话失败", true);
        }
    }

