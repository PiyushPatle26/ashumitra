
// Global state
let currentMode = 'fill'; // 'fill' or 'dispense'
let filledDoses = [];

// DOM Elements
document.addEventListener('DOMContentLoaded', () => {
    // Initialize mode selector
    const modeSelector = document.getElementById('mode-selector');
    modeSelector.addEventListener('change', (e) => {
        currentMode = e.target.value;
        updateUI();
    });

    // Initialize day and dose selectors
    const daySelect = document.getElementById('day-select');
    const doseSelect = document.getElementById('dose-select');
    
    // Add action button listener
    const actionButton = document.getElementById('action-button');
    actionButton.addEventListener('click', handleAction);

    // Initial UI update
    updateFilledDoses();
    updateUI();
});

// UI Updates
function updateUI() {
    const actionButton = document.getElementById('action-button');
    const controlsTitle = document.getElementById('controls-title');
    
    if (currentMode === 'fill') {
        actionButton.textContent = 'Add Dose';
        controlsTitle.textContent = 'Fill Mode';
        actionButton.classList.remove('dispense');
        actionButton.classList.add('fill');
    } else {
        actionButton.textContent = 'Dispense';
        controlsTitle.textContent = 'Dispense Mode';
        actionButton.classList.remove('fill');
        actionButton.classList.add('dispense');
    }
}

async function updateFilledDoses() {
    try {
        const response = await fetch('/get_filled_doses');
        const data = await response.json();
        filledDoses = data.filled_doses;
        
        // Update the filled doses display
        const filledDosesList = document.getElementById('filled-doses-list');
        filledDosesList.innerHTML = '';
        
        filledDoses.forEach(dose => {
            const doseElement = document.createElement('div');
            doseElement.classList.add('filled-dose');
            
            const dayText = dose.day.charAt(0).toUpperCase() + dose.day.slice(1);
            doseElement.textContent = `${dayText} - Dose ${dose.dose}`;
            
            const removeButton = document.createElement('button');
            removeButton.textContent = 'Remove';
            removeButton.classList.add('remove-dose');
            removeButton.addEventListener('click', () => removeDose(dose.day, dose.dose));
            
            doseElement.appendChild(removeButton);
            filledDosesList.appendChild(doseElement);
        });
    } catch (error) {
        showError('Failed to fetch filled doses');
    }
}

// Action Handlers
async function handleAction() {
    const daySelect = document.getElementById('day-select');
    const doseSelect = document.getElementById('dose-select');
    
    const day = daySelect.value.toLowerCase();
    const dose = parseInt(doseSelect.value);
    
    if (currentMode === 'fill') {
        await addDose(day, dose);
    } else {
        await dispenseDose(day, dose);
    }
}

async function addDose(day, dose) {
    try {
        const response = await fetch('/add_dose', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({ day, dose }),
        });
        
        const data = await response.json();
        if (data.success) {
            showSuccess(data.message);
            updateFilledDoses();
        } else {
            showError(data.message);
        }
    } catch (error) {
        showError('Failed to add dose');
    }
}

async function removeDose(day, dose) {
    try {
        const response = await fetch('/remove_dose', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({ day, dose }),
        });
        
        const data = await response.json();
        if (data.success) {
            showSuccess('Dose removed successfully');
            updateFilledDoses();
        } else {
            showError(data.message);
        }
    } catch (error) {
        showError('Failed to remove dose');
    }
}

async function dispenseDose(day, dose) {
    try {
        const response = await fetch('/dispense', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({ day, dose }),
        });
        
        const data = await response.json();
        if (data.success) {
            showSuccess('Pill dispensed successfully');
            updateFilledDoses();
        } else {
            showError(data.message);
        }
    } catch (error) {
        showError('Failed to dispense pill');
    }
}

// Utility Functions
function showError(message) {
    const notification = document.getElementById('notification');
    notification.textContent = message;
    notification.className = 'notification error';
    setTimeout(() => {
        notification.className = 'notification';
    }, 3000);
}

function showSuccess(message) {
    const notification = document.getElementById('notification');
    notification.textContent = message;
    notification.className = 'notification success';
    setTimeout(() => {
        notification.className = 'notification';
    }, 3000);
}