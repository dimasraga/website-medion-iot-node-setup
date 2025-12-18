/* system_setting.js (Versi Bersih) */
document.addEventListener("DOMContentLoaded", function () {
    const elements = {
        submitSettings: document.getElementById('submitSettings'),
        datetime: document.getElementById('datetime'),
        currentDatetime: document.getElementById('currentDatetime'),
        popup: document.querySelector('.popup'),
        headPopup: document.getElementById('headPopup'),
        textPopup: document.getElementById('textPopup'),
        username: document.getElementById('username'),
        password: document.getElementById('password'),
        sdInterval: document.getElementById('sdInterval'),
        settingsForm: document.getElementById('settingsForm')
    };

    // Logic Home Link & Theme Toggle SUDAH DIHAPUS (Pindah ke common.js)

    elements.submitSettings.addEventListener('click', () => {
        const interval = parseInt(elements.sdInterval.value);
        if (interval < 1 || interval > 1440) {
            alert('SD Card interval must be between 1 and 1440 minutes');
            return;
        }
        var formData = new FormData(elements.settingsForm);
        submitForm(formData);
    });

    // Load settings
    fetch('/settingsLoad', { method: "GET" })
        .then(response => response.json())
        .then(data => {
            elements.username.value = data.username;
            elements.password.value = data.password;
            elements.sdInterval.value = data.sdInterval || 5;
        })
        .catch(error => console.error("Error:", error));

    function getTime() {
        fetch('/getTime', { method: "GET" })
            .then(response => response.json())
            .then(data => elements.currentDatetime.textContent = data.datetime)
            .catch(console.error);
    }

    function submitForm(formData) {
        var xhr = new XMLHttpRequest();
        xhr.open('POST', '/');
        xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
        xhr.onreadystatechange = function () {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                elements.popup.style.display = 'block';
                if (xhr.status === 200) {
                    elements.headPopup.innerHTML = '<i class="fas fa-check-circle"></i> Form Submitted!';
                    elements.textPopup.innerHTML = 'Settings saved successfully.';
                } else {
                    elements.headPopup.innerHTML = '<i class="fas fa-times-circle"></i> Submit Failed';
                    elements.textPopup.innerHTML = 'Please fill the correct input';
                }
            }
        };
        xhr.send(new URLSearchParams(formData).toString());
    }

    setInterval(getTime, 500);
});