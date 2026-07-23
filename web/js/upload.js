        async function uploadFile(input) {
            const token = getAuthToken();
            if (!token) { showToast("请先登录", true); return; }

            const file = input.files[0];
            if (!file) return;

            const originalName = encodeURIComponent(file.name);
            const CHUNK_SIZE = runtimeConfig.uploadChunkSize;
            const UPLOAD_CONCURRENCY = runtimeConfig.uploadConcurrency;
            const totalChunks = Math.ceil(file.size / CHUNK_SIZE);

            showToast("正在计算文件校验和...", false);

            try {
                const fileHash = await calculateFileHash(file, (processed, total) => {
                    const percent = total === 0 ? 100 : Math.floor(processed * 100 / total);
                    showToast(`正在计算文件校验和 ${percent}%...`, false);
                });

                showToast("正在校验文件状态...", false);

                // 1. 初始化上传：检查是否秒传，或需要哪些分片
                const initRes = await fetch(
                    `${API_BASE}/api/upload/init?hash=${fileHash}&total=${totalChunks}&size=${file.size}&chunkSize=${CHUNK_SIZE}`,
                    {
                    headers: { 'Authorization': `Bearer ${token}` }
                    }
                );
                const initData = await readUploadResponse(initRes, "校验文件状态");

                if (initData.status === "exists") { // 秒传逻辑由合并接口处理或在此截断
                    // 秒传可以直接调用 merge 让后端处理元数据
                    const instantMergeRes = await fetch(`${API_BASE}/api/upload/merge`, {
                        method: 'POST',
                        headers: {
                            'Authorization': `Bearer ${token}`, 'File-Name': originalName,
                            'File-Hash': fileHash, 'Total-Chunks': totalChunks, 'File-Size': file.size,
                            'Directory': encodeURIComponent(window.SmartNASState.currentDirectory)
                        }
                    });
                    await readUploadResponse(instantMergeRes, "创建秒传记录");
                    showToast("上传结果: Hit! Seconds-Transfer success.");
                    fetchFileList();
                    if (isSummarizableFile(file.name)) startSummarizeByHash(fileHash, { silent: true });
                    input.value = '';
                    return;
                }

                // 2. 上传缺失的分片
                const missingChunks = initData.missing || [];
                let nextChunk = 0;
                let completedChunks = 0;
                const uploadWorker = async () => {
                    while (nextChunk < missingChunks.length) {
                        const listIndex = nextChunk++;
                        const chunkIndex = missingChunks[listIndex];
                        const start = chunkIndex * CHUNK_SIZE;
                        const end = Math.min(start + CHUNK_SIZE, file.size);
                        const chunkRes = await fetch(`${API_BASE}/api/upload/chunk`, {
                            method: 'POST',
                            headers: {
                                'File-Hash': fileHash,
                                'Chunk-Index': chunkIndex,
                                'Authorization': `Bearer ${token}`
                            },
                            body: file.slice(start, end)
                        });
                        await readUploadResponse(chunkRes, `上传分片 ${chunkIndex + 1}`);
                        completedChunks++;
                        showToast(`正在上传分片 ${completedChunks}/${missingChunks.length}...`);
                    }
                };
                await Promise.all(Array.from(
                    { length: Math.min(UPLOAD_CONCURRENCY, missingChunks.length) },
                    uploadWorker
                ));

                showToast("合并文件...");

                // 3. 通知后端合并分片
                const mergeRes = await fetch(`${API_BASE}/api/upload/merge`, {
                    method: 'POST',
                    headers: {
                        'Authorization': `Bearer ${token}`, 'File-Name': originalName,
                        'File-Hash': fileHash, 'Total-Chunks': totalChunks, 'File-Size': file.size,
                        'Directory': encodeURIComponent(window.SmartNASState.currentDirectory)
                    }
                });
                const mergeData = await readUploadResponse(mergeRes, "合并文件");
                showToast("上传结果: " + mergeData.message);
                fetchFileList();
                if (isSummarizableFile(file.name)) startSummarizeByHash(fileHash, { silent: true });
            } catch (e) {
                console.error(e);
                showToast(`上传过程出错: ${e.message || e}`, true);
            } finally {
                input.value = '';
            }
        }

        async function readUploadResponse(response, stage) {
            const text = await response.text();
            let data = null;
            if (text) {
                try {
                    data = JSON.parse(text);
                } catch (error) {
                    if (response.ok) {
                        throw new Error(`${stage}失败: 服务器返回了无效响应`);
                    }
                }
            }

            if (!response.ok) {
                const detail = data?.error || data?.message || text || response.statusText || "未知错误";
                throw new Error(`${stage}失败 (${response.status}): ${detail}`);
            }
            return data || {};
        }

        const SHA256_CONSTANTS = new Uint32Array([
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        ]);

        class Sha256 {
            constructor() {
                this.state = new Uint32Array([
                    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
                ]);
                this.buffer = new Uint8Array(64);
                this.words = new Uint32Array(64);
                this.bufferLength = 0;
                this.bytesHashed = 0;
                this.finished = false;
            }

            update(data) {
                if (this.finished) throw new Error("SHA-256 hasher is already finalized");

                this.bytesHashed += data.length;
                let position = 0;

                if (this.bufferLength > 0) {
                    const needed = 64 - this.bufferLength;
                    const available = Math.min(needed, data.length);
                    this.buffer.set(data.subarray(0, available), this.bufferLength);
                    this.bufferLength += available;
                    position += available;
                    if (this.bufferLength === 64) {
                        this.hashBlock(this.buffer, 0);
                        this.bufferLength = 0;
                    }
                }

                while (position + 64 <= data.length) {
                    this.hashBlock(data, position);
                    position += 64;
                }

                if (position < data.length) {
                    this.buffer.set(data.subarray(position), 0);
                    this.bufferLength = data.length - position;
                }
                return this;
            }

            hashBlock(data, offset) {
                const words = this.words;
                for (let i = 0; i < 16; i++) {
                    const j = offset + i * 4;
                    words[i] = (
                        (data[j] << 24) |
                        (data[j + 1] << 16) |
                        (data[j + 2] << 8) |
                        data[j + 3]
                    ) >>> 0;
                }

                for (let i = 16; i < 64; i++) {
                    const x = words[i - 15];
                    const y = words[i - 2];
                    const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
                    const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
                    words[i] = (words[i - 16] + s0 + words[i - 7] + s1) >>> 0;
                }

                let [a, b, c, d, e, f, g, h] = this.state;
                for (let i = 0; i < 64; i++) {
                    const sum1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
                    const choice = (e & f) ^ (~e & g);
                    const temp1 = (h + sum1 + choice + SHA256_CONSTANTS[i] + words[i]) >>> 0;
                    const sum0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
                    const majority = (a & b) ^ (a & c) ^ (b & c);
                    const temp2 = (sum0 + majority) >>> 0;

                    h = g;
                    g = f;
                    f = e;
                    e = (d + temp1) >>> 0;
                    d = c;
                    c = b;
                    b = a;
                    a = (temp1 + temp2) >>> 0;
                }

                this.state[0] = (this.state[0] + a) >>> 0;
                this.state[1] = (this.state[1] + b) >>> 0;
                this.state[2] = (this.state[2] + c) >>> 0;
                this.state[3] = (this.state[3] + d) >>> 0;
                this.state[4] = (this.state[4] + e) >>> 0;
                this.state[5] = (this.state[5] + f) >>> 0;
                this.state[6] = (this.state[6] + g) >>> 0;
                this.state[7] = (this.state[7] + h) >>> 0;
            }

            digest() {
                if (!this.finished) {
                    const bytesHashed = this.bytesHashed;
                    let length = this.bufferLength;
                    this.buffer[length++] = 0x80;

                    if (length > 56) {
                        this.buffer.fill(0, length, 64);
                        this.hashBlock(this.buffer, 0);
                        length = 0;
                    }
                    this.buffer.fill(0, length, 56);

                    const bitLengthHigh = Math.floor(bytesHashed / 0x20000000);
                    const bitLengthLow = (bytesHashed << 3) >>> 0;
                    this.buffer[56] = bitLengthHigh >>> 24;
                    this.buffer[57] = bitLengthHigh >>> 16;
                    this.buffer[58] = bitLengthHigh >>> 8;
                    this.buffer[59] = bitLengthHigh;
                    this.buffer[60] = bitLengthLow >>> 24;
                    this.buffer[61] = bitLengthLow >>> 16;
                    this.buffer[62] = bitLengthLow >>> 8;
                    this.buffer[63] = bitLengthLow;
                    this.hashBlock(this.buffer, 0);
                    this.finished = true;
                }

                const result = new Uint8Array(32);
                for (let i = 0; i < this.state.length; i++) {
                    result[i * 4] = this.state[i] >>> 24;
                    result[i * 4 + 1] = this.state[i] >>> 16;
                    result[i * 4 + 2] = this.state[i] >>> 8;
                    result[i * 4 + 3] = this.state[i];
                }
                return result;
            }

            hex() {
                return Array.from(this.digest())
                    .map(byte => byte.toString(16).padStart(2, '0'))
                    .join('');
            }
        }

        async function calculateFileHash(file, onProgress) {
            // 中小文件交给浏览器原生加密实现，通常显著快于 JS/WASM 增量循环。
            const WEB_CRYPTO_LIMIT = runtimeConfig.webCryptoLimit;
            if (globalThis.crypto?.subtle && file.size <= WEB_CRYPTO_LIMIT) {
                const digest = await globalThis.crypto.subtle.digest("SHA-256", await file.arrayBuffer());
                if (onProgress) onProgress(file.size, file.size);
                return Array.from(new Uint8Array(digest))
                    .map(byte => byte.toString(16).padStart(2, '0'))
                    .join('');
            }

            const wasmAvailable = Boolean(globalThis.hashwasm?.createSHA256);
            const HASH_CHUNK_SIZE = wasmAvailable ? 16 * 1024 * 1024 : 4 * 1024 * 1024;
            const hasher = wasmAvailable ? await globalThis.hashwasm.createSHA256() : new Sha256();
            if (wasmAvailable) hasher.init();

            let pendingRead = file.size > 0
                ? file.slice(0, Math.min(HASH_CHUNK_SIZE, file.size)).arrayBuffer()
                : null;
            for (let offset = 0; offset < file.size; offset += HASH_CHUNK_SIZE) {
                const end = Math.min(offset + HASH_CHUNK_SIZE, file.size);
                const chunk = new Uint8Array(await pendingRead);
                const nextEnd = Math.min(end + HASH_CHUNK_SIZE, file.size);
                pendingRead = end < file.size ? file.slice(end, nextEnd).arrayBuffer() : null;
                hasher.update(chunk);
                if (onProgress) onProgress(end, file.size);
            }

            if (file.size === 0 && onProgress) onProgress(0, 0);
            return wasmAvailable ? hasher.digest("hex") : hasher.hex();
        }
