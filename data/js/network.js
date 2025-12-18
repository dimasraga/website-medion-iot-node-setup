document.addEventListener("DOMContentLoaded", function () {
  // Definisi Element
  var sendTrig = document.getElementById("sendTrig");
  var loggerMode = document.getElementById("loggerMode");
  var modbusMode = document.getElementById("modbusMode");
  var modbusPort = document.getElementById("modbusPort");
  var protocolMode2 = document.getElementById("protocolMode2");
  var modbusSlaveID = document.getElementById("modbusSlaveID");
  var networkMode = document.getElementById("networkMode");
  var ssid = document.getElementById("ssid");
  var pass = document.getElementById("password");
  var apSsid = document.getElementById("apSsid");
  var apPass = document.getElementById("apPassword");
  var dhcpMode = document.getElementById("dhcpMode");
  var ipAddress = document.getElementById("ipAddress");
  var subnet = document.getElementById("subnet");
  var ipGateway = document.getElementById("ipGateway");
  var ipDNS = document.getElementById("ipDNS");
  var sendInterval = document.getElementById("sendInterval");
  var protocolMode = document.getElementById("protocolMode");
  var endpoint = document.getElementById("endpoint");
  var port = document.getElementById("port");
  var pubTopic = document.getElementById("pubTopic");
  var subTopic = document.getElementById("subTopic");
  var mqttUsername = document.getElementById("mqttUsername");
  var mqttPass = document.getElementById("mqttPass");
  var netIntForm = document.getElementById('net-interface');
  var submitNetwork = document.getElementById('submitNetwork');

  // Popups
  var popupSubmit = document.getElementById('popupSubmit');
  var popupRestart = document.getElementById('popupRestart');
  var popupInformation = document.getElementById('popupInformation');
  var buttonPopupInt = document.getElementById('buttonPopupInt');
  var buttonRestart = document.getElementById('buttonRestart');
  var buttonYes = document.getElementById('buttonYes');
  var buttonNo = document.getElementById('buttonNo');
  var buttonRedirect = document.getElementById('buttonRedirect');
  var headPopup = document.getElementById('headPopup');
  var textPopup = document.getElementById('textPopup');

  // ERP
  var erpInterface = document.getElementById('erp-interface');
  var erpUrl = document.getElementById('erpUrl');
  var erpUsername = document.getElementById('erpUsername');
  var erpPassword = document.getElementById('erpPassword');
  var submitErp = document.getElementById('submitErp');

  const baseURL = window.location.protocol + '//' + window.location.hostname;

  // --- Event Listeners untuk Mode ---

  loggerMode.addEventListener('change', function () {
    var mode = this.checked;
    protocolMode.disabled = !mode;
    endpoint.disabled = !mode;
    port.disabled = !mode;
    pubTopic.disabled = !mode;
    subTopic.disabled = !mode;
    mqttUsername.disabled = !mode;
    mqttPass.disabled = !mode;
    sendInterval.disabled = !mode;
  });

  modbusMode.addEventListener('change', function () {
    var mode = this.checked;
    protocolMode2.disabled = !mode;
    modbusSlaveID.disabled = !mode;
    modbusPort.disabled = !mode;
  });

  // --- Load Data Awal ---
  fetch('/networkLoad', { method: "GET" })
    .then(response => response.json())
    .then(data => {
      networkMode.value = data.networkMode;
      ssid.value = data.ssid;
      pass.value = data.password;
      apSsid.value = data.apSsid;
      apPass.value = data.apPassword;
      dhcpMode.value = data.dhcpMode;
      ipAddress.value = data.ipAddress;
      subnet.value = data.subnet;
      ipGateway.value = data.ipGateway;
      ipDNS.value = data.ipDNS;
      sendInterval.value = data.sendInterval;
      protocolMode.value = data.protocolMode;
      endpoint.value = data.endpoint;
      port.value = data.port;
      pubTopic.value = data.pubTopic;
      subTopic.value = data.subTopic;
      mqttUsername.value = data.mqttUsername;
      mqttPass.value = data.mqttPass;
      loggerMode.checked = data.loggerMode;
      modbusMode.checked = data.modbusMode;
      protocolMode2.value = data.protocolMode2;
      modbusPort.value = data.modbusPort;
      modbusSlaveID.value = data.modbusSlaveID;
      sendTrig.value = data.sendTrig;

      // ERP
      if (erpUrl) erpUrl.value = data.erpUrl || '';
      if (erpUsername) erpUsername.value = data.erpUsername || '';
      if (erpPassword) erpPassword.value = data.erpPassword || '';

      // Disable fields based on data
      disableFormFields(networkMode.value);
      disableFormFields(dhcpMode.value);
      disableFormFields(protocolMode.value);
      disableFormFields(modbusMode.value);
      disableFormFields(protocolMode2.value);
      disableFormFields(sendTrig.value);

      // Trigger change event manual agar checkbox logic jalan
      loggerMode.dispatchEvent(new Event('change'));
      modbusMode.dispatchEvent(new Event('change'));
    })
    .catch(error => {
      console.error("An error occurred:", error);
    });

  function disableFormFields(selectionMode) {
    if (selectionMode === 'DHCP') {
      ipAddress.disabled = true;
      subnet.disabled = true;
      ipGateway.disabled = true;
      ipDNS.disabled = true;
    } else if (selectionMode === 'Static') {
      ipAddress.disabled = false;
      subnet.disabled = false;
      ipGateway.disabled = false;
      ipDNS.disabled = false;
    }
    if (selectionMode === 'Ethernet') {
      ssid.disabled = true;
      pass.disabled = true;
    } else if (selectionMode === 'WiFi') {
      ssid.disabled = false;
      pass.disabled = false;
    }
    if (selectionMode === 'HTTP') {
      pubTopic.disabled = true;
      subTopic.disabled = true;
    } else if (selectionMode === 'MQTT') {
      pubTopic.disabled = false;
      subTopic.disabled = false;
    }
    if (selectionMode && selectionMode.includes('Rising Edge')) {
      sendInterval.disabled = true;
    } else if (selectionMode === 'Time/interval') {
      sendInterval.disabled = false;
    }
  }

  // --- Helper Change Listeners ---
  networkMode.addEventListener('change', function () { disableFormFields(this.value); });
  dhcpMode.addEventListener('change', function () { disableFormFields(this.value); });
  sendTrig.addEventListener('change', function () { disableFormFields(this.value); });
  protocolMode.addEventListener('change', function () { disableFormFields(this.value); });

  // --- Validasi IP Visual ---
  ipAddress.addEventListener('input', function (event) {
    if (ipAddress.validity.valid) {
      ipAddress.classList.remove('invalid');
    } else {
      ipAddress.classList.add('invalid');
    }
  });

  // --- Submit Forms ---
  submitNetwork.addEventListener('click', function (event) {
    var formData = new FormData(netIntForm);
    submitForm(formData);
  });

  submitErp.addEventListener('click', function (event) {
    // Mencegah submit default jika button type='submit'
    event.preventDefault();
    var formData = new FormData(erpInterface);
    submitForm(formData);
  });

  function submitForm(formData) {
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/');
    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    xhr.onreadystatechange = function () {
      if (xhr.readyState === XMLHttpRequest.DONE) {
        popupSubmit.style.display = 'block';
        if (xhr.status === 200) {
          headPopup.innerHTML = 'Form Submitted!';
          textPopup.innerHTML = 'Thank you for submitting the form.';
        } else {
          headPopup.innerHTML = 'Submit Failed';
          textPopup.innerHTML = 'Please fill the correct input';
        }
      }
    };
    xhr.send(new URLSearchParams(formData).toString());
  }
  if (buttonPopupInt) {
    buttonPopupInt.addEventListener('click', function () {
      popupSubmit.style.display = 'none';
    });
  }

  if (buttonRedirect) {
    buttonRedirect.addEventListener('click', function () {
      popupInformation.style.display = 'none';
      window.location.assign(baseURL);
    });
  }

  if (buttonRestart) {
    buttonRestart.addEventListener('click', function () {
      popupRestart.style.display = 'block';
    });
  }

  if (buttonNo) {
    buttonNo.addEventListener('click', function () {
      popupRestart.style.display = 'none';
    });
  }

  if (buttonYes) {
    buttonYes.addEventListener('click', function () {
      popupRestart.style.display = 'none';
      fetch('/networkLoad?restart=1', { method: "GET" })
        .then(response => {
          if (response.ok) {
            popupInformation.style.display = 'block';
          }
        })
        .catch(error => console.error("Error restarting:", error));
    });
  }
});