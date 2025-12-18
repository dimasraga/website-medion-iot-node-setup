document.addEventListener("DOMContentLoaded", function () {
    
    // --- Variabel UI ---
    var parameterList = document.getElementById('parameterList');
    var baudrate = document.getElementById('baudrate');
    var parity = document.getElementById('parity');
    var stopBit = document.getElementById('stopBit');
    var dataBit = document.getElementById('dataBit');
    var scanRate = document.getElementById('scanRate');
    var saveSetup = document.getElementById('saveSetup');
    var saveParam = document.getElementById('saveParam');
    var addParam = document.getElementById('addParam');
    var delParam = document.getElementById('delParam');
    var popup = document.querySelector('.popup');
    var buttonPopupInt = document.getElementById('buttonPopupInt');
    var headPopup = document.getElementById('headPopup');
    var textPopup = document.getElementById('textPopup');
    
    // Variabel Data
    let modbusData;
    var deviceAddress = document.getElementById('deviceAddress');
    var functionCode = document.getElementById('functionCode');
    var registerAddress = document.getElementById('registerAddress');
    var paramName = document.getElementById('paramName');
    var multiplier = document.getElementById('multiplier');
    var realtimeValue = document.getElementById('realtimeValue');
    var offsetAddress = document.getElementById('offsetAddress');

    // --- Load Data Awal ---
    fetch('/modbusLoad', { method: "GET" })
        .then(response => response.json())
        .then(data => {
            modbusData = data;
            loadModbusData(modbusData);
        })
        .catch(error => console.error("An error occurred:", error));

    // --- Event Listeners ---

    // Simpan Konfigurasi Serial
    saveSetup.addEventListener('click', function (event) {
        modbusData.baudrate = parseInt(baudrate.value);
        modbusData.parity = parity.value;
        modbusData.stopBit = parseInt(stopBit.value);
        modbusData.dataBit = parseInt(dataBit.value);
        modbusData.scanRate = parseFloat(scanRate.value);
        submitForm();
    });

    // Update Parameter yang Dipilih
    saveParam.addEventListener('click', function (event) {
        // Update serial config juga agar sinkron
        modbusData.baudrate = parseInt(baudrate.value);
        modbusData.parity = parity.value;
        modbusData.stopBit = parseInt(stopBit.value);
        modbusData.dataBit = parseInt(dataBit.value);
        modbusData.scanRate = parseFloat(scanRate.value);

        // Logic ganti nama parameter
        if (parameterList.value !== paramName.value) {
            const indexToRemove = modbusData.nameData.indexOf(parameterList.value);
            delete (modbusData[parameterList.value]);
            modbusData.nameData.splice(indexToRemove, 1);
            modbusData.nameData.push(paramName.value);
            
            // Update dropdown option text
            for (let i = 0; i < parameterList.options.length; i++) {
                if (parameterList.options[i].value === parameterList.value) {
                    parameterList.options[i].textContent = paramName.value;
                    parameterList.options[i].value = paramName.value;
                    break; 
                }
            }
            parameterList.value = paramName.value;
        }

        // Simpan data parameter baru
        modbusData[paramName.value] = [
            parseInt(deviceAddress.value), 
            parseInt(functionCode.value), 
            parseInt(registerAddress.value), 
            parseFloat(multiplier.value), 
            parseInt(offsetAddress.value)
        ];
        submitForm();
    });

    // Tambah Parameter Baru
    addParam.addEventListener('click', function (event) {
        if(!paramName.value) {
            alert("Parameter Name cannot be empty");
            return;
        }
        modbusData.nameData.push(paramName.value);
        const optionElement = document.createElement('option');
        optionElement.textContent = paramName.value;
        optionElement.value = paramName.value;
        parameterList.appendChild(optionElement);
        
        modbusData[paramName.value] = [
            deviceAddress.value, 
            functionCode.value, 
            registerAddress.value, 
            multiplier.value, 
            offsetAddress.value
        ];
        // Set dropdown ke item baru
        parameterList.value = paramName.value; 
        submitForm();
    });

    // Hapus Parameter
    delParam.addEventListener('click', function (event) {
        const selectedOption = parameterList.options[parameterList.selectedIndex];
        if (selectedOption) {
            const paramNameToDelete = selectedOption.value;
            const indexToRemove = modbusData.nameData.indexOf(paramNameToDelete);
            
            // Hapus dari UI
            parameterList.removeChild(selectedOption);
            
            // Hapus dari Data
            if (indexToRemove > -1) {
                modbusData.nameData.splice(indexToRemove, 1);
            }
            delete modbusData[paramNameToDelete];
            
            // Bersihkan Form
            paramName.value = "";
            deviceAddress.value = "";
            functionCode.value = "";
            registerAddress.value = "";
            multiplier.value = "";
            offsetAddress.value = "";

            submitForm();
        } else {
            alert('Please select an option to delete.');
        }
    });

    // Listener saat Dropdown Berubah
    parameterList.addEventListener('change', function (event) {
        populateFormFromSelection(parameterList.value);
    });

    // Helper: Populate Form
    function populateFormFromSelection(key) {
        if (modbusData && modbusData[key]) {
            paramName.value = key;
            deviceAddress.value = modbusData[key][0];
            functionCode.value = modbusData[key][1];
            registerAddress.value = modbusData[key][2];
            multiplier.value = modbusData[key][3];
            offsetAddress.value = modbusData[key][4];
        }
    }

    // --- Fungsi Logic Utama ---

    function loadModbusData(jsonObject) {
        baudrate.value = jsonObject.baudrate;
        parity.value = jsonObject.parity;
        stopBit.value = jsonObject.stopBit;
        dataBit.value = jsonObject.dataBit;
        scanRate.value = jsonObject.scanRate;

        // Clear existing options first to prevent duplicates on reload
        parameterList.innerHTML = "";

        jsonObject.nameData.forEach(param => {
            const optionElement = document.createElement('option');
            optionElement.textContent = param;
            optionElement.value = param;
            parameterList.appendChild(optionElement);
        });

        // Trigger form population for the first item if exists
        if (jsonObject.nameData.length > 0) {
            parameterList.value = jsonObject.nameData[0];
            populateFormFromSelection(parameterList.value);
        }
    }

    function submitForm() {
        const url = '/modbus_setup';
        const requestData = {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(modbusData)
        };

        fetch(url, requestData)
            .then(response => {
                popup.style.display = 'block';
                if (response.ok) {
                    headPopup.innerHTML = 'Form Submitted!';
                    textPopup.innerHTML = 'Configuration saved successfully.';
                } else {
                    headPopup.innerHTML = 'Submit Failed';
                    textPopup.innerHTML = 'Please fill the correct input.';
                }
            })
            .catch(error => console.error('An error occurred:', error));
    }

    function getModbusReading() {
        fetch('/getValue', { method: "GET" })
            .then(response => response.json())
            .then(data => {
                let found = false;
                for (let i = 0; i < data.length; i++) {
                    if (data[i].KodeSensor === parameterList.value) {
                        realtimeValue.innerHTML = Math.round(data[i].Value * 100) / 100;
                        found = true;
                        break;
                    }
                }
                if (!found) realtimeValue.innerHTML = "-";
            })
            .catch(error => console.error("Polling error:", error));
    }
    
    // Tombol Close Popup (meskipun ada di common.js, di sini ada logic display: none spesifik)
    if(buttonPopupInt) {
        buttonPopupInt.addEventListener('click', function () {
            popup.style.display = 'none';
        });
    }

    // Polling setiap 2 detik
    setInterval(getModbusReading, 2000);
});