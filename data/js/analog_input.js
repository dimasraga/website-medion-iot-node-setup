document.addEventListener("DOMContentLoaded", function () {

    // --- Variabel UI ---
    var analogForm = document.getElementById('analogIO');
    var submitAnalog = document.getElementById('submitAnalog');

    var filter = document.getElementById('filter');
    var filterPeriod = document.getElementById('filterPeriod');

    var scaling = document.getElementById('scaling');
    var lowLimit = document.getElementById('lowLimit');
    var highLimit = document.getElementById('highLimit');

    var calibration = document.getElementById('calibration');
    var mValue = document.getElementById('mValue');
    var cValue = document.getElementById('cValue');

    var inputType = document.getElementById('inputType');
    var inputPin = document.getElementById('inputPin');
    var sensorName = document.getElementById('name');

    var popup = document.querySelector('.popup');
    // Tombol close popup sudah ditangani oleh common.js, 
    // tapi kita butuh referensinya untuk manipulasi manual jika perlu.
    var headPopup = document.getElementById('headPopup');
    var textPopup = document.getElementById('textPopup');

    // --- Event Listeners untuk Enable/Disable Field ---

    filter.addEventListener('change', function () {
        filterPeriod.disabled = !this.checked;
    });

    scaling.addEventListener('change', function () {
        var mode = this.checked;
        lowLimit.disabled = !mode;
        highLimit.disabled = !mode;
    });

    calibration.addEventListener('change', function () {
        var mode = this.checked;
        mValue.disabled = !mode;
        cValue.disabled = !mode;
    });

    // --- Submit Form menggunakan Fetch API (Modern) ---
    submitAnalog.addEventListener('click', function (event) {
        var formData = new FormData(analogForm);
        submitForm(formData);
    });

    function submitForm(formData) {
        // Debugging
        for (var pair of formData.entries()) {
            console.log(pair[0] + ': ' + pair[1]);
        }

        // Menggunakan URLSearchParams untuk format x-www-form-urlencoded
        const params = new URLSearchParams(formData);

        fetch('/?' + params.toString(), {
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded'
            },
            body: params // Beberapa backend support body di POST, beberapa baca query params
        })
            .then(response => {
                popup.style.display = 'block';
                if (response.ok) { // Status 200-299
                    headPopup.innerHTML = 'Form Submitted!';
                    textPopup.innerHTML = 'Configuration saved successfully.';
                } else {
                    headPopup.innerHTML = 'Submit Failed';
                    textPopup.innerHTML = 'Please fill the correct input.';
                }
            })
            .catch(error => {
                console.error('Error:', error);
                popup.style.display = 'block';
                headPopup.innerHTML = 'Error';
                textPopup.innerHTML = 'An error occurred while saving.';
            });
    }

    // --- Load Data Awal ---

    // Load default (AI1 - index ke-2 dari string "AI1" adalah 1)
    loadAnalogData(inputPin.value[2]);

    // Listener saat dropdown Input Pin berubah
    inputPin.addEventListener('change', function () {
        // Mengambil karakter ke-3 (index 2) dari string "AI1", "AI2", dst.
        var pinIndex = this.value[2];
        loadAnalogData(pinIndex);
    });

    function loadAnalogData(index) {
        fetch('/analogLoad?input=' + index, { method: "GET" })
            .then(response => response.json())
            .then(data => {
                // Populate Form
                inputType.value = data.inputType;
                sensorName.value = data.name;

                // Checkboxes & Fields
                filter.checked = data.filter;
                filterPeriod.value = data.filterPeriod;
                filterPeriod.disabled = !data.filter;

                scaling.checked = data.scaling;
                lowLimit.value = data.lowLimit;
                highLimit.value = data.highLimit;
                lowLimit.disabled = !data.scaling;
                highLimit.disabled = !data.scaling;

                calibration.checked = data.calibration;
                // Perbaikan: Data JSON harusnya mengembalikan mValue/cValue jika ada
                // Pastikan properti JSON dari server sesuai (mValue vs mvalue)
                mValue.value = data.mValue !== undefined ? data.mValue : "";
                cValue.value = data.cValue !== undefined ? data.cValue : "";
                mValue.disabled = !data.calibration;
                cValue.disabled = !data.calibration;
            })
            .catch(error => {
                console.error("Error loading data:", error);
            });
    }
});