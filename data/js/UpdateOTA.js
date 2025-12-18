document.addEventListener("DOMContentLoaded", function () {
  let selectedFile = null;
  const uploadArea = document.getElementById('uploadArea');
  const fileInput = document.getElementById('fileInput');
  const uploadBtn = document.getElementById('uploadBtn');
  const fileInfo = document.getElementById('fileInfo'); // Tambahan referensi
  const progressBar = document.getElementById('progressBar');
  const progressBarInner = document.getElementById('progressBarInner');

  // --- System Info Functions ---

  function loadSystemInfo() {
    fetch('/updateStatus')
      .then(response => response.json())
      .then(data => {
        document.getElementById('freeHeap').textContent = formatBytes(data.freeHeap);
        document.getElementById('sketchSize').textContent = formatBytes(data.sketchSize);
        document.getElementById('freeSketchSpace').textContent = formatBytes(data.freeSketchSpace);
      })
      .catch(err => {
        console.error('Failed to load system info:', err);
      });
  }

  function formatBytes(bytes) {
    if (bytes === 0) return '0 Bytes';
    const k = 1024;
    const sizes = ['Bytes', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return Math.round((bytes / Math.pow(k, i)) * 100) / 100 + ' ' + sizes[i];
  }

  function showAlert(message, type = 'info') {
    const alertContainer = document.getElementById('alertContainer');
    const alert = document.createElement('div');
    alert.className = `alert alert-${type} alert-dismissible fade show`;
    alert.innerHTML = `
                  ${message}
                  <button type="button" class="btn-close" data-bs-dismiss="alert"></button>
              `;
    alertContainer.innerHTML = '';
    alertContainer.appendChild(alert);
  }

  // --- File Handling Events ---

  uploadArea.addEventListener('click', () => fileInput.click());

  uploadArea.addEventListener('dragover', (e) => {
    e.preventDefault();
    uploadArea.classList.add('dragging');
  });

  uploadArea.addEventListener('dragleave', () => {
    uploadArea.classList.remove('dragging');
  });

  uploadArea.addEventListener('drop', (e) => {
    e.preventDefault();
    uploadArea.classList.remove('dragging');
    const files = e.dataTransfer.files;
    if (files.length > 0) {
      handleFileSelect(files[0]);
    }
  });

  fileInput.addEventListener('change', (e) => {
    if (e.target.files.length > 0) {
      handleFileSelect(e.target.files[0]);
    }
  });


  function handleFileSelect(file) {
    if (!file.name.endsWith('.bin')) {
      showAlert('Please select a .bin file!', 'danger');
      return;
    }

    selectedFile = file;

    // Update UI info
    document.getElementById('fileName').textContent = file.name;
    document.getElementById('fileSize').textContent = formatBytes(file.size);

    const updateType = file.name.toLowerCase().includes('spiffs') ? 'SPIFFS' : 'Firmware';
    document.getElementById('updateType').textContent = updateType;

    fileInfo.style.display = 'block'; // Tampilkan info file
    uploadBtn.disabled = false;

    showAlert('File selected successfully! Click "Upload Firmware" to start.', 'success');
  }

  // --- Upload Logic ---

  uploadBtn.addEventListener('click', async () => {
    if (!selectedFile) return;

    uploadBtn.disabled = true;
    if (progressBar) progressBar.style.display = 'flex'; // Tampilkan progress bar

    showAlert('Uploading firmware... Please do not close this page.', 'warning');

    const formData = new FormData();
    formData.append('update', selectedFile);

    try {
      const xhr = new XMLHttpRequest();

      xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
          const percentComplete = Math.round((e.loaded / e.total) * 100);
          progressBarInner.style.width = percentComplete + '%';
          progressBarInner.textContent = percentComplete + '%';
        }
      });

      xhr.addEventListener('load', () => {
        if (xhr.status === 200) {
          progressBarInner.classList.remove('progress-bar-animated');
          progressBarInner.classList.add('bg-success');
          showAlert('✅ Update successful! Device is rebooting...', 'success');

          // Countdown and redirect logic
          let countdown = 10;
          const countdownInterval = setInterval(() => {
            countdown--;
            if (countdown <= 0) {
              clearInterval(countdownInterval);
              window.location.href = '/';
            } else {
              showAlert(`✅ Update successful! Redirecting in ${countdown} seconds...`, 'success');
            }
          }, 1000);
        } else {
          throw new Error('Upload failed');
        }
      });

      xhr.addEventListener('error', () => {
        progressBarInner.classList.remove('progress-bar-animated');
        progressBarInner.classList.add('bg-danger');
        showAlert('❌ Upload failed! Connection error.', 'danger');
        uploadBtn.disabled = false;
      });

      xhr.open('POST', '/update');
      xhr.send(formData);

    } catch (error) {
      console.error('Upload error:', error);
      showAlert('❌ Upload failed: ' + error.message, 'danger');
      uploadBtn.disabled = false;
    }
  });

  // Init
  loadSystemInfo();
  setInterval(loadSystemInfo, 5000);
});