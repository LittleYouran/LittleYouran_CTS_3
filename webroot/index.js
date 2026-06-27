let currentMode = 'balance';
let currentCpuModel = '未知';
let currentEditingFile = null;
let autoRefreshInterval = null;
let actualConfigFiles = [];

class CpuTurboSchedulerControl {
    constructor() {
        this.configDir = '/sdcard/Android/CTS';
        this.logFile = `${this.configDir}/log.txt`;
        this.modeFile = `${this.configDir}/mode.txt`;
        this.init();
    }

    async init() {
        this.bindEvents();
        await this.initializePage();
        this.startAutoRefresh();
        await this.loadLogs();
    }

    async exec(cmd) {
        return new Promise((resolve) => {
            const callback = `cb_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
            let timeout = setTimeout(() => {
                delete window[callback];
                resolve('');
            }, 5000);
            
            window[callback] = (code, stdout, stderr) => {
                clearTimeout(timeout);
                delete window[callback];
                resolve(stdout ? stdout.trim() : '');
            };
            
            try {
                if (typeof ksu !== 'undefined' && ksu.exec) {
                    ksu.exec(cmd, "{}", callback);
                } else {
                    console.log('KSU not available, executing locally:', cmd);
                    setTimeout(() => {
                        window[callback](0, "模拟输出", "");
                    }, 100);
                }
            } catch (e) {
                console.error('Execution error:', e);
                clearTimeout(timeout);
                delete window[callback];
                resolve('');
            }
        });
    }

    async detectCpuModel() {
        try {
            const socModelResult = await this.exec('getprop ro.soc.model');
            if (socModelResult && socModelResult.trim()) {
                const socModel = socModelResult.trim().toUpperCase();
                console.log('检测到处理器:', socModel);
                return socModel;
            }
            
            const propKeys = [
                'ro.board.platform',
                'ro.chipname', 
                'ro.product.oplus.cpuinfo',
                'ro.hardware.chipname',
                'ro.mediatek.platform',
                'ro.hardware'
            ];
            
            for (const propKey of propKeys) {
                const result = await this.exec(`getprop ${propKey}`);
                if (result && result.trim()) {
                    const propValue = result.trim().toUpperCase();
                    
                    if (propValue.includes('SM') || propValue.includes('MT') || 
                        propValue.includes('EXYNOS') || propValue.includes('KIRIN') ||
                        propValue.includes('UNISOC') || propValue.includes('SPRD')) {
                        
                        console.log(`通过属性 ${propKey} 检测到处理器:`, propValue);
                        
                        if (propValue.includes('SM')) {
                            const match = propValue.match(/SM[0-9]+P?/);
                            if (match) return match[0];
                        } else if (propValue.includes('MT')) {
                            const match = propValue.match(/MT[0-9]+/);
                            if (match) return match[0];
                        }
                        
                        return propValue;
                    }
                }
            }
            
            console.warn('无法准确检测CPU型号，使用默认值');
            return '未知型号';
            
        } catch (error) {
            console.error('CPU型号检测错误:', error);
            return '检测失败';
        }
    }

    async scanConfigFiles() {
        try {
            console.log('扫描配置文件目录:', this.configDir);
            
            const dirExists = await this.exec(`[ -d "${this.configDir}" ] && echo "exists"`);
            if (!dirExists || !dirExists.includes('exists')) {
                console.warn('配置文件目录不存在');
                return [];
            }
            
            const filesResult = await this.exec(`ls "${this.configDir}"`);
            
            if (!filesResult) {
                console.log('目录为空');
                return [];
            }
            
            const filePaths = filesResult.split('\n').filter(path => path.trim());
            const scannedFiles = [];
            
            const fileDescriptions = {
                'config.json': '主配置文件 (CPU频率/调速器/功能开关)',
                'perapp_powermode.txt': '分应用性能模式配置',
                'mode.txt': '当前运行模式',
                'log.txt': '服务运行日志'
            };
            
            const fileIcons = {
                '.json': '📊',
                '.txt': '📝',
                'config.json': '⚙️',
                'perapp_powermode.txt': '📱',
                'log.txt': '📋',
                'mode.txt': '🔘'
            };
            
            for (const filePath of filePaths) {
                const fileName = filePath.split('/').pop();
                const fileExt = fileName.includes('.') ? fileName.split('.').pop() : '';
                
                let fileType = fileExt;
                let icon = fileIcons[`.${fileExt}`] || '📄';
                
                if (fileName === 'config.json') {
                    fileType = 'config';
                    icon = fileIcons['config.json'];
                } else if (fileName === 'log.txt') {
                    fileType = 'log';
                    icon = fileIcons['log.txt'];
                } else if (fileName === 'mode.txt') {
                    fileType = 'mode';
                    icon = fileIcons['mode.txt'];
                } else if (fileName === 'perapp_powermode.txt') {
                    fileType = 'perapp';
                    icon = fileIcons['perapp_powermode.txt'];
                }
                
                let description = fileDescriptions[fileName] || `${fileName} 配置文件`;
                
                scannedFiles.push({
                    name: fileName,
                    path: `${this.configDir}/${fileName}`,
                    type: fileType,
                    description: description,
                    icon: icon,
                    editable: !(fileName === 'log.txt')
                });
            }
            
            console.log(`扫描到 ${scannedFiles.length} 个配置文件`);
            return scannedFiles;
            
        } catch (error) {
            console.error('扫描配置文件错误:', error);
            return [];
        }
    }

    async readFileContent(filePath) {
        try {
            console.log('读取文件:', filePath);
            const b64 = await this.exec(`base64 "${filePath}" 2>/dev/null | tr -d '\\n'`);
            
            if (b64) {
                try {
                    return decodeURIComponent(escape(atob(b64)));
                } catch(e) {
                    return atob(b64);
                }
            }
            return '';
        } catch (error) {
            console.warn('读取文件错误:', error.message);
            return '';
        }
    }

    async writeFileContent(filePath, content) {
        try {
            console.log('写入文件:', filePath);
            
            const dirPath = filePath.substring(0, filePath.lastIndexOf('/'));
            await this.exec(`mkdir -p "${dirPath}"`);
            
            const b64 = btoa(unescape(encodeURIComponent(content)));
            await this.exec(`printf '%s' '${b64}' | base64 -d > "${filePath}" && chmod 644 "${filePath}"`);
            
            return true;
        } catch (error) {
            console.error('写入文件错误:', error);
            throw error;
        }
    }

    async checkFileExists(filePath) {
        try {
            const result = await this.exec(`[ -f "${filePath}" ] && echo "exists" || echo "not exists"`);
            return result.includes('exists');
        } catch (error) {
            console.warn('检查文件存在错误:', error.message);
            return false;
        }
    }

    async loadCurrentMode() {
        try {
            const modeFilePath = this.modeFile;
            const exists = await this.checkFileExists(modeFilePath);
            
            if (exists) {
                const content = await this.readFileContent(modeFilePath);
                const savedMode = content.split('\n')[0].trim();
                
                if (savedMode && ['powersave', 'balance', 'performance', 'fast'].includes(savedMode)) {
                    await this.setMode(savedMode, false);
                    return;
                }
            }
            
            await this.setMode('balance', false);
            
        } catch (error) {
            console.error('加载当前模式错误:', error);
            await this.setMode('balance', false);
        }
    }

    async setMode(mode, showToast = true) {
        const originalMode = currentMode;
        currentMode = mode;
        
        try {
            document.getElementById('current-mode').textContent = this.getModeName(mode);
            document.getElementById('mode-status').textContent = this.getModeStatus(mode);
            
            const modeIndicator = document.getElementById('mode-indicator');
            modeIndicator.className = `mode-dot mode-${mode}`;
            
            document.querySelectorAll('.mode-button').forEach(btn => {
                const btnMode = btn.getAttribute('data-mode');
                if (btnMode) {
                    btn.classList.toggle('active', btnMode === mode);
                }
            });
            
            await this.writeFileContent(this.modeFile, mode);
            
            if (showToast) {
                this.showToast(`已切换到${this.getModeName(mode)}模式`, 'success');
            }
            
        } catch (error) {
            currentMode = originalMode;
            document.getElementById('current-mode').textContent = this.getModeName(originalMode);
            
            const modeIndicator = document.getElementById('mode-indicator');
            modeIndicator.className = `mode-dot mode-${originalMode}`;
            
            document.querySelectorAll('.mode-button').forEach(btn => {
                const btnMode = btn.getAttribute('data-mode');
                if (btnMode) {
                    btn.classList.toggle('active', btnMode === originalMode);
                }
            });
            
            console.error('切换模式错误:', error);
            if (showToast) {
                this.showToast(`模式切换失败: ${error.message}`, 'error');
            }
        }
    }

    getModeName(mode) {
        const names = {
            'powersave': '省电',
            'balance': '均衡',
            'performance': '性能',
            'fast': '极速'
        };
        return names[mode] || mode;
    }

    getModeStatus(mode) {
        const status = {
            'powersave': '节能运行',
            'balance': '平衡运行',
            'performance': '高性能',
            'fast': '极速模式'
        };
        return status[mode] || mode;
    }

    async renderFileList() {
        const fileList = document.getElementById('file-list');
        const searchTerm = document.getElementById('search-input')?.value.toLowerCase() || '';
        
        if (!fileList) return;
        
        fileList.innerHTML = '<div class="loading">加载中...</div>';
        
        try {
            actualConfigFiles = await this.scanConfigFiles();
            
            const filteredFiles = actualConfigFiles.filter(file => 
                file.name.toLowerCase().includes(searchTerm) || 
                file.description.toLowerCase().includes(searchTerm)
            );
            
            if (filteredFiles.length === 0) {
                fileList.innerHTML = '<div class="loading">未找到配置文件</div>';
                return;
            }
            
            const filesWithStatus = await Promise.all(
                filteredFiles.map(async (file) => {
                    try {
                        const exists = await this.checkFileExists(file.path);
                        return { ...file, exists };
                    } catch (error) {
                        console.warn(`检查文件 ${file.name} 存在状态失败:`, error);
                        return { ...file, exists: false };
                    }
                })
            );
            
            fileList.innerHTML = '';
            
            filesWithStatus.forEach(file => {
                const fileItem = document.createElement('div');
                fileItem.className = 'file-item';
                fileItem.setAttribute('data-filename', file.name);
                fileItem.innerHTML = `
                    <div class="file-info">
                        <div class="file-name">${file.icon || '📄'} ${file.name} ${!file.exists ? '<span style="color: #F44336; font-size: 10px;">(不存在)</span>' : ''}</div>
                        <div class="file-description">${file.description}</div>
                    </div>
                    <div class="file-action" data-filename="${file.name}">
                        ${file.editable ? '编辑' : '查看'}
                    </div>
                `;
                fileList.appendChild(fileItem);
            });
            
            document.getElementById('config-count').textContent = `${actualConfigFiles.length} 个文件`;
            
            document.querySelectorAll('.file-item').forEach(item => {
                item.addEventListener('click', (e) => {
                    const fileName = item.getAttribute('data-filename');
                    if (fileName) {
                        this.openFileEditor(fileName);
                    }
                });
            });
            
        } catch (error) {
            console.error('渲染文件列表错误:', error);
            fileList.innerHTML = '<div class="loading">加载失败: ' + error.message + '</div>';
        }
    }

    async openFileEditor(fileName) {
        if (!fileName) {
            this.showToast('文件名无效', 'error');
            return;
        }
        
        const file = actualConfigFiles.find(f => f.name === fileName);
        if (!file) {
            this.showToast('未找到文件: ' + fileName, 'error');
            return;
        }
        
        currentEditingFile = file;
        document.getElementById('editor-title').textContent = `编辑 ${file.name}`;
        
        try {
            const editor = document.getElementById('file-editor');
            if (editor) {
                editor.value = '正在加载文件内容...';
                editor.readOnly = true;
            }
            
            const exists = await this.checkFileExists(file.path);
            let content = '';
            
            if (exists) {
                content = await this.readFileContent(file.path);
            } else {
                content = `# 文件不存在: ${file.path}\n# 将创建新文件\n\n# 在此处输入配置内容`;
            }
            
            if (editor) {
                editor.value = content;
                editor.readOnly = !file.editable;
            }
            
            const saveBtn = document.getElementById('save-file');
            if (saveBtn) {
                saveBtn.style.display = file.editable ? 'block' : 'none';
            }
            
            const editorContainer = document.getElementById('editor-container');
            if (editorContainer) {
                editorContainer.style.display = 'block';
            }
            
        } catch (error) {
            console.error('打开文件编辑器错误:', error);
            const editor = document.getElementById('file-editor');
            if (editor) {
                editor.value = `# 读取文件失败: ${error.message}\n# 文件路径: ${file.path}\n\n# 请检查文件权限或路径是否正确`;
            }
            
            const editorContainer = document.getElementById('editor-container');
            if (editorContainer) {
                editorContainer.style.display = 'block';
            }
        }
    }

    closeEditor() {
        const editorContainer = document.getElementById('editor-container');
        if (editorContainer) {
            editorContainer.style.display = 'none';
        }
        currentEditingFile = null;
    }

    async saveFile() {
        if (!currentEditingFile) {
            this.showToast('没有正在编辑的文件', 'error');
            return;
        }
        
        const editor = document.getElementById('file-editor');
        if (!editor) {
            this.showToast('编辑器未找到', 'error');
            return;
        }
        
        const content = editor.value;
        const saveBtn = document.getElementById('save-file');
        const originalText = saveBtn ? saveBtn.textContent : '保存';
        
        const editingFile = currentEditingFile;
        
        try {
            if (saveBtn) {
                saveBtn.textContent = '保存中...';
                saveBtn.disabled = true;
            }
            
            if (editingFile.name === 'mode.txt') {
                const firstLine = content.split('\n')[0].trim();
                await this.writeFileContent(editingFile.path, firstLine);
                await this.setMode(firstLine);
            } else {
                let finalContent = content;
                if (editingFile.name === 'perapp_powermode.txt') {
                    const lines = content.split('\n');
                    const autoIdx = lines.findIndex(l => /^\*\s+/.test(l.trim()));
                    if (autoIdx >= 0) {
                        const [autoLine] = lines.splice(autoIdx, 1);
                        let insertIdx = 0;
                        for (let i = 0; i < lines.length; i++) {
                            if (lines[i].trim().startsWith('#') || lines[i].trim() === '') {
                                insertIdx = i + 1;
                            } else {
                                break;
                            }
                        }
                        lines.splice(insertIdx, 0, autoLine);
                        finalContent = lines.join('\n');
                        editor.value = finalContent;
                    }
                }
                await this.writeFileContent(editingFile.path, finalContent);
            }
            
            this.showToast(`文件 ${editingFile.name} 保存成功`, 'success');
            
            this.closeEditor();
            await this.renderFileList();
            
        } catch (error) {
            console.error('保存文件错误:', error);
            this.showToast(`保存失败: ${error.message}`, 'error');
        } finally {
            if (saveBtn) {
                saveBtn.textContent = originalText;
                saveBtn.disabled = false;
            }
        }
    }

    async loadLogs() {
        try {
            const logContent = await this.readFileContent(this.logFile);
            const logElement = document.getElementById('live-log');
            if (logContent) {
                const coloredLog = logContent
                    .replace(/信息/g, '<span style="color:#2d7d46;">信息</span>')
                    .replace(/警告/g, '<span style="color:#ffa726;">警告</span>')
                    .replace(/错误/g, '<span style="color:#ff6b6b;">错误</span>')
                    .replace(/调试/g, '<span style="color:#4dabf7;">调试</span>')
                    .replace(/INFO/g, '<span style="color:#2d7d46;">INFO</span>')
                    .replace(/WARN/g, '<span style="color:#ffa726;">WARN</span>')
                    .replace(/ERROR/g, '<span style="color:#ff6b6b;">ERROR</span>')
                    .replace(/DEBUG/g, '<span style="color:#4dabf7;">DEBUG</span>');
                logElement.innerHTML = coloredLog;
            } else {
                logElement.textContent = '暂无日志数据';
            }
            logElement.scrollTop = logElement.scrollHeight;
        } catch (e) {
            console.error('Failed to load logs:', e);
            document.getElementById('live-log').textContent = '读取日志失败，请检查权限';
        }
    }

    async clearLogs() {
        try {
            await this.writeFileContent(this.logFile, '');
            await this.loadLogs();
            this.showToast('日志已清空', 'success');
        } catch (error) {
            this.showToast('清空日志失败', 'error');
        }
    }

    showToast(msg, type = 'info') {
        const toast = document.getElementById('toast');
        toast.textContent = msg;
        
        const colors = {
            'info': 'rgba(44, 94, 158, 0.9)',
            'success': '#4CAF50',
            'error': '#F44336',
            'warning': '#FF9800'
        };
        
        toast.style.backgroundColor = colors[type] || colors.info;
        toast.classList.add('show');
        
        setTimeout(() => {
            toast.classList.remove('show');
        }, 3000);
    }

    bindEvents() {
        document.querySelectorAll('.tab-item').forEach(tab => {
            tab.addEventListener('click', (e) => {
                const page = e.currentTarget.dataset.page;
                this.switchPage(page);
            });
        });

        document.querySelectorAll('.mode-button').forEach(button => {
            button.addEventListener('click', (e) => {
                const mode = e.currentTarget.dataset.mode;
                if (mode) {
                    this.setMode(mode);
                }
            });
        });

        document.getElementById('btn-apply-mode')?.addEventListener('click', () => {
            this.setMode(currentMode);
        });

        document.getElementById('btn-refresh')?.addEventListener('click', async () => {
            await this.refreshStatus();
            this.showToast('数据已刷新', 'success');
        });

        const searchInput = document.getElementById('search-input');
        if (searchInput) {
            searchInput.addEventListener('input', () => {
                setTimeout(() => this.renderFileList(), 300);
            });
        }

        document.getElementById('cancel-edit')?.addEventListener('click', () => {
            this.closeEditor();
        });
        
        document.getElementById('save-file')?.addEventListener('click', () => {
            this.saveFile();
        });

        document.getElementById('btn-clear-log')?.addEventListener('click', () => {
            this.clearLogs();
        });

        document.getElementById('btn-load-logs')?.addEventListener('click', () => {
            this.loadLogs();
            this.showToast('日志已刷新', 'success');
        });

        const themeToggle = document.getElementById('theme-toggle');
        if (themeToggle) {
            const savedTheme = localStorage.getItem('theme') || 'light';
            themeToggle.checked = savedTheme === 'dark';
            
            this.applyTheme(savedTheme === 'dark');
            
            themeToggle.addEventListener('change', (e) => {
                const isDark = e.target.checked;
                this.applyTheme(isDark);
                localStorage.setItem('theme', isDark ? 'dark' : 'light');
            });
        }

        const autoRefreshToggle = document.getElementById('auto-refresh-toggle');
        if (autoRefreshToggle) {
            autoRefreshToggle.addEventListener('change', () => {
                this.toggleAutoRefresh(autoRefreshToggle.checked);
            });
        }
    }

    applyTheme(isDark) {
        if (isDark) {
            document.body.classList.add('dark-theme');
        } else {
            document.body.classList.remove('dark-theme');
        }
    }

    switchPage(pageId) {
        document.querySelectorAll('.ui-content').forEach(el => el.classList.add('hidden'));
        document.getElementById(`page-${pageId}`).classList.remove('hidden');
        
        document.querySelectorAll('.tab-item').forEach(el => el.classList.remove('active'));
        document.querySelector(`.tab-item[data-page="${pageId}"]`).classList.add('active');
        
        switch (pageId) {
            case 'monitor':
                this.refreshStatus();
                break;
            case 'logs':
                this.loadLogs();
                break;
            case 'control':
                this.renderFileList();
                break;
        }
    }

    toggleAutoRefresh(enabled) {
        if (enabled) {
            this.startAutoRefresh();
            localStorage.setItem('autoRefresh', 'true');
        } else {
            this.stopAutoRefresh();
            localStorage.setItem('autoRefresh', 'false');
        }
    }

    startAutoRefresh() {
        this.stopAutoRefresh();
        autoRefreshInterval = setInterval(() => {
            const currentPage = document.querySelector('.tab-item.active').dataset.page;
            if (currentPage === 'monitor') {
                this.refreshStatus();
            } else if (currentPage === 'logs') {
                this.loadLogs();
            }
        }, 5000);
    }

    stopAutoRefresh() {
        if (autoRefreshInterval) {
            clearInterval(autoRefreshInterval);
            autoRefreshInterval = null;
        }
    }

    async refreshStatus() {
        try {
            document.getElementById('cpu-model').textContent = currentCpuModel;
            document.getElementById('config-count').textContent = `${actualConfigFiles.length} 个文件`;
        } catch (error) {
            console.error('刷新状态错误:', error);
        }
    }

    async initializePage() {
        try {
            currentCpuModel = await this.detectCpuModel();
            
            await this.loadCurrentMode();
            
            await this.renderFileList();
            
            await this.refreshStatus();
            
            const autoRefresh = localStorage.getItem('autoRefresh') !== 'false';
            const autoRefreshToggle = document.getElementById('auto-refresh-toggle');
            if (autoRefreshToggle) {
                autoRefreshToggle.checked = autoRefresh;
                if (autoRefresh) {
                    this.startAutoRefresh();
                }
            }
            
            this.showToast('已就绪', 'success');
            
        } catch (error) {
            console.error('初始化页面错误:', error);
            this.showToast('页面初始化失败: ' + error.message, 'error');
        }
    }
}

document.addEventListener('DOMContentLoaded', () => {
    window.cpuTurboSchedulerControl = new CpuTurboSchedulerControl();
});
