/* js/digital_IO.js */

document.addEventListener("DOMContentLoaded", function () {

    // --- LOGIC TEMA & NAVIGASI DIHAPUS (Diurus common.js) ---

    // Definisi Element
    const elements = {
        submitDigital: document.getElementById('submitDigital'),
        digitalForm: document.getElementById('digitalIO'),
        // Popup elements
        popup: document.querySelector('.popup'),
        buttonPopupInt: document.getElementById('buttonPopupInt'),
        headPopup: document.getElementById('headPopup'),
        textPopup: document.getElementById('textPopup'),
        // Form elements
        inputPin: document.getElementById('inputPin'),
        taskMode: document.getElementById('taskMode'),
        inputInversion: document.getElementById('inputInversion'),
        inputInversionDiv: document.getElementById('inputInversionDiv'),
        inputState: document.getElementById('inputState'),
        inputStateDiv: document.getElementById('inputStateDiv'),
        resetValue: document.getElementById('resetValue'),
        currentValue: document.getElementById('currentValue'),
        unit: document.getElementById('unit'),
        nameDI: document.getElementById('nameDI'),
        // Dynamic Divs
        intervalTime: document.getElementById('intervalTimeDiv'),
        intervalTimeValue: document.getElementById('intervalTime'),
        conversionFactor: document.getElementById('conversionFactorDiv'),
        conversionFactorValue: document.getElementById('conversionFactor')
    };

    // --- Event Listeners ---

    // 1. Saat Pin Dipilih Berubah
    elements.inputPin.addEventListener('change', () => {
        fetchData(elements.inputPin.value[2]); // Ambil karakter ke-3 (DI'1', DI'2')
    });

    // 2. Saat Task Mode Berubah (Normal vs Counting vs Pulse dll)
    elements.taskMode.addEventListener('change', () => {
        updateTaskMode(elements.taskMode.value);
    });

    // 3. Tombol Reset Value (Counter/Timer)
    elements.resetValue.addEventListener('click', () => {
        fetch(`/digitalLoad?reset=${elements.inputPin.value[2]}`, { method: "GET" })
            .then(() => alert("Value reset successfully"))
            .catch(e => console.error("Reset failed", e));
    });

    // 4. Tombol Submit
    elements.submitDigital.addEventListener('click', () => {
        const formData = new FormData(elements.digitalForm);
        submitForm(formData);
    });

    // --- Functions ---

    function fetchData(val) {
        fetch(`/digitalLoad?input=${val}`, { method: "GET" })
            .then(response => response.json())
            .then(data => {
                populateForm(data);
                updateTaskMode(elements.taskMode.value);
            })
            .catch(error => console.error("Fetch config error:", error));
    }

    function populateForm(data) {
        elements.nameDI.value = data.nameDI || '';
        elements.inputInversion.checked = data.invDI || false;
        elements.taskMode.value = data.taskMode || 'Normal';
        elements.inputState.value = data.inputState || 'High';
        elements.intervalTimeValue.value = data.intervalTime || 0;
        elements.conversionFactorValue.value = data.conversionFactor || 1;
    }

    // Logika Utama: Menyembunyikan/Menampilkan field berdasarkan Mode
    function updateTaskMode(selectedTaskMode) {
        const isNormal = selectedTaskMode === "Normal";
        const isCycleTime = selectedTaskMode === "Cycle Time";
        const isCounting = selectedTaskMode === "Counting";
        const isRunTime = selectedTaskMode === "Run Time";
        const isPulseMode = selectedTaskMode === "Pulse Mode";

        // Tampilkan Inversion hanya di Normal
        elements.inputInversionDiv.style.display = isNormal ? "block" : "none";

        // Tampilkan Active State di Normal & Run Time
        elements.inputStateDiv.style.display = (isNormal || isRunTime) ? "block" : "none";

        // Reset hanya aktif jika bukan Cycle Time (bisa disesuaikan kebutuhan)
        elements.resetValue.disabled = isCycleTime;

        // Pulse Mode Butuh Interval & Conversion Factor
        elements.intervalTime.style.display = isPulseMode ? "block" : "none";
        elements.conversionFactor.style.display = isPulseMode ? "block" : "none";
    }

    function getSensorReading() {
        fetch('/getValue', { method: "GET" })
            .then(response => response.json())
            .then(data => {
                // Cari data sensor berdasarkan Nama Sensor (Sensor Code)
                const sensor = data.find(s => s.KodeSensor === elements.nameDI.value);
                elements.currentValue.value = sensor ? sensor.Value : '-';

                // Update Unit Satuan secara dinamis
                switch (elements.taskMode.value) {
                    case "Normal": elements.unit.innerHTML = "."; break;
                    case "Cycle Time": elements.unit.innerHTML = "sec"; break;
                    case "Run Time": elements.unit.innerHTML = "min"; break;
                    case "Counting": elements.unit.innerHTML = "pcs"; break;
                    case "Pulse Mode": elements.unit.innerHTML = "Hz"; break;
                    default: elements.unit.innerHTML = "-";
                }
            })
            .catch(error => console.error("Polling error:", error));
    }

    function submitForm(formData) {
        fetch('/', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: new URLSearchParams(formData)
        })
            .then(response => {
                elements.popup.style.display = 'block';
                if (response.ok) {
                    elements.headPopup.innerHTML = 'Form Submitted!';
                    elements.textPopup.innerHTML = 'Configuration saved successfully.';
                } else {
                    throw new Error("Server returned error");
                }
            })
            .catch(() => {
                elements.popup.style.display = 'block';
                elements.headPopup.innerHTML = 'Submit Failed';
                elements.textPopup.innerHTML = 'Please check your input and connection.';
            });
    }

    // Tombol Close Popup (Specific Handler jika ID sama dengan common.js)
    // Karena common.js sudah handle 'buttonPopupInt', kita tidak wajib pasang listener di sini
    // KECUALI jika Anda ingin logika display:none manual. 
    // Common.js sudah cukup untuk menutup popup standard.

    // --- Init ---
    // Load data awal untuk DI1 (default)
    if (elements.inputPin.value) {
        fetchData(elements.inputPin.value[2]);
    }

    // Mulai Polling
    setInterval(getSensorReading, 2000);
});