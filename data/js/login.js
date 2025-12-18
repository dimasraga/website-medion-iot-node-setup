document.addEventListener("DOMContentLoaded", function () {
    
    // 1. Cek parameter error di URL (contoh: /login?error=1)
    const urlParams = new URLSearchParams(window.location.search);
    const errorMsg = document.getElementById('errorMessage');
    
    if (urlParams.get('error') === '1') {
        if(errorMsg) errorMsg.classList.add('show');
        
        // Bersihkan URL agar terlihat rapi
        window.history.replaceState({}, document.title, '/login');
    }

    // 2. Handle Submit Form
    const loginForm = document.getElementById('loginForm');
    const loginBtn = document.getElementById('loginBtn');

    if(loginForm) {
        loginForm.addEventListener('submit', function (e) {
            // Ubah tombol jadi loading agar user tahu sedang proses
            if(loginBtn) {
                loginBtn.disabled = true;
                loginBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> Signing in...';
            }
            
            // Sembunyikan pesan error saat mencoba login lagi
            if(errorMsg) errorMsg.classList.remove('show');
        });
    }

    // 3. Hapus pesan error jika user mulai mengetik ulang
    const inputs = document.querySelectorAll('input');
    inputs.forEach(input => {
        input.addEventListener('input', function () {
            if(errorMsg) errorMsg.classList.remove('show');
        });
    });
});