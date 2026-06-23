        async function showMarkdownByHash(hash) {
            const token = getAuthToken();
            if (!token) return;
            showToast("正在读取提取文本...");
            try {
                const res = await fetch(`${AGENT_BASE}/api/agent/markdown/${encodeURIComponent(hash)}`, {
                    headers: { 'Authorization': `Bearer ${token}` }
                });
                const data = await res.json();
                if (!res.ok) throw new Error(data.detail || '读取失败');
                const win = window.open('', '_blank');
                win.document.write(`<pre style="white-space:pre-wrap;font:14px/1.6 ui-monospace,monospace;padding:24px;color:#1f2937">${escapeHtml(data.markdown)}</pre>`);
            } catch (e) {
                showToast(`读取提取文本失败: ${e.message}`, true);
            }
        }

        function askFileByHash(hash) {
            const token = getAuthToken();
            if (!token) return;
            pendingAskFileHash = hash;
            const questionInput = document.getElementById('askFileQuestion');
            const result = document.getElementById('askFileResult');
            questionInput.value = '';
            result.textContent = '';
            result.classList.add('hidden');
            result.classList.remove('text-red-600', 'border-red-100', 'bg-red-50');
            openModal('askFileModal');
            setTimeout(() => questionInput.focus(), 0);
        }

        async function submitAskFile() {
            const token = getAuthToken();
            const question = document.getElementById('askFileQuestion').value.trim();
            const hash = pendingAskFileHash;
            if (!question) return;
            const button = document.getElementById('askFileSubmitBtn');
            const result = document.getElementById('askFileResult');
            button.disabled = true;
            button.classList.add('opacity-60');
            button.innerHTML = '<i class="fas fa-spinner fa-spin mr-2"></i>正在回答';
            result.textContent = '正在读取文件并组织答案…';
            result.classList.remove('hidden');
            result.classList.remove('text-red-600', 'border-red-100', 'bg-red-50');
            try {
                const res = await fetch(`${AGENT_BASE}/api/agent/file_qa/stream`, {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                        'Authorization': `Bearer ${token}`
                    },
                    body: JSON.stringify({ hash, question })
                });
                if (!res.ok) {
                    let detail = `文件问答返回 ${res.status}`;
                    try {
                        const data = await res.json();
                        detail = data.detail || data.message || detail;
                    } catch (_) {}
                    throw new Error(detail);
                }
                if (!res.body) throw new Error('浏览器不支持流式响应');

                result.textContent = '';
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
                            result.textContent = answer;
                            result.scrollTop = result.scrollHeight;
                        } else if (data.type === 'error') {
                            throw new Error(data.message || '文件问答流式生成失败');
                        }
                    }
                    if (done) break;
                }
                if (!answer) result.textContent = '没有得到有效回答。';
            } catch (e) {
                result.textContent = `文件问答失败：${e.message}`;
                result.classList.add('text-red-600', 'border-red-100', 'bg-red-50');
            } finally {
                button.disabled = false;
                button.classList.remove('opacity-60');
                button.innerHTML = '<i class="fas fa-paper-plane mr-2"></i>再次提问';
            }
        }
