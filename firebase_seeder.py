#!/usr/bin/env python3
"""
Firebase Realtime Database Seeder for Battery Balancing System
DYNAMIC BALANCING SIMULATION - Batteries charge TOGETHER with automatic load balancing
para pantay-pantay ang percentage ng lahat ng batteries.

Goal: Maintain equal percentage across all 3 batteries
- If a battery is ahead (higher %), reduce its charging current
- If a battery is behind (lower %), increase its charging current
- All batteries charge simultaneously but at different rates to achieve balance
"""

import firebase_admin
from firebase_admin import credentials, db
import random
import time
import datetime
import argparse
import signal
import sys

# ==================== CONFIGURATION ====================
SERVICE_ACCOUNT_KEY_PATH = "serviceAccountKey.json"
DATABASE_URL = "https://smart-battery-b06ae-default-rtdb.asia-southeast1.firebasedatabase.app/"

UPDATE_INTERVAL = 10  # seconds between updates
HISTORY_INTERVAL = 60
ENABLE_HISTORY = True

DEFAULT_CHARGE_TIME_MINUTES = 30  # Approximate time to full charge
DEFAULT_MAX_CHARGE_CYCLES = 3
DEFAULT_IMBALANCE_TOLERANCE = 5  # Max allowed % difference before balancing kicks in

# ==================== INITIALIZE FIREBASE ====================
def initialize_firebase():
    try:
        cred = credentials.Certificate(SERVICE_ACCOUNT_KEY_PATH)
        firebase_admin.initialize_app(cred, {
            'databaseURL': DATABASE_URL
        })
        print("✅ Firebase initialized successfully!")
        return db.reference('/')
    except Exception as e:
        print(f"❌ Failed to initialize Firebase: {e}")
        print("\nMake sure serviceAccountKey.json is in the correct location")
        sys.exit(1)

# ==================== BATTERY PARAMETERS ====================
BATTERY_PARAMS = {
    'b1': {
        'volt_min': 3.0,
        'volt_max': 4.2,
        'current_max': 2.0,
        'name': '3.7V Li-ion',
        'base_charge_rate': 2.0,  # Base charging rate (% per update)
        'balancing_factor': 1.0    # Will be adjusted dynamically
    },
    'b2': {
        'volt_min': 5.4,
        'volt_max': 6.8,
        'current_max': 3.0,
        'name': '6V SLA',
        'base_charge_rate': 2.5,
        'balancing_factor': 1.0
    },
    'b3': {
        'volt_min': 10.0,
        'volt_max': 14.4,
        'current_max': 5.0,
        'name': '12V LiFePO4',
        'base_charge_rate': 3.0,
        'balancing_factor': 1.0
    }
}

# ==================== HELPER FUNCTIONS ====================
def percentage_to_voltage(percentage, v_min, v_max):
    """Convert percentage to voltage"""
    return round(v_min + (percentage / 100.0) * (v_max - v_min), 2)

def get_charging_current(percentage, max_current, balancing_factor):
    """
    Calculate charging current based on:
    1. Battery percentage (CC/CV profile)
    2. Balancing factor (higher factor = more current)
    """
    if percentage >= 95:
        # CV mode - tapering
        factor = (100 - percentage) / 5.0
        base_current = max_current * factor * 0.3
    elif percentage >= 80:
        # Absorption stage
        base_current = max_current * 0.8
    else:
        # Bulk charge
        base_current = max_current
    
    # Apply balancing factor
    # balancing_factor > 1 = charge FASTER (catching up)
    # balancing_factor < 1 = charge SLOWER (letting others catch up)
    current = base_current * balancing_factor
    
    return round(max(0.05, min(max_current, current)), 2)

def get_temperature(percentage, ambient_temp, current, charging):
    """Generate realistic battery temperature"""
    base_temp = ambient_temp
    if charging:
        heat = abs(current) * 3
    else:
        heat = abs(current) * 1.5
    
    if percentage > 90 or percentage < 10:
        heat *= 1.5
    
    temp = base_temp + heat + random.uniform(-0.5, 0.5)
    return round(min(55, max(ambient_temp, temp)), 1)

def calculate_balancing_factors(percentages):
    """
    Calculate balancing factors based on current percentages.
    
    Goal: Make all percentages equal.
    - Battery with LOWEST percentage gets HIGHEST factor (charge faster)
    - Battery with HIGHEST percentage gets LOWEST factor (charge slower)
    
    Returns: dictionary of balancing factors for b1, b2, b3
    """
    if not percentages:
        return {'b1': 1.0, 'b2': 1.0, 'b3': 1.0}
    
    # Get current percentages
    p1 = percentages.get('b1', 50)
    p2 = percentages.get('b2', 50)
    p3 = percentages.get('b3', 50)
    
    # Calculate target (average)
    target = (p1 + p2 + p3) / 3.0
    
    # Calculate how far each battery is from target
    diff1 = target - p1  # Positive = behind, Negative = ahead
    diff2 = target - p2
    diff3 = target - p3
    
    # Convert difference to balancing factor
    # Behind (positive diff) = factor > 1 (charge faster)
    # Ahead (negative diff) = factor < 1 (charge slower)
    
    # Maximum adjustment: ±30% (factor between 0.7 and 1.3)
    MAX_ADJUST = 0.3
    
    factor1 = 1.0 + (diff1 / 100.0) * 2  # 2% behind = 1.04 factor, 2% ahead = 0.96 factor
    factor1 = max(1.0 - MAX_ADJUST, min(1.0 + MAX_ADJUST, factor1))
    
    factor2 = 1.0 + (diff2 / 100.0) * 2
    factor2 = max(1.0 - MAX_ADJUST, min(1.0 + MAX_ADJUST, factor2))
    
    factor3 = 1.0 + (diff3 / 100.0) * 2
    factor3 = max(1.0 - MAX_ADJUST, min(1.0 + MAX_ADJUST, factor3))
    
    return {
        'b1': round(factor1, 2),
        'b2': round(factor2, 2),
        'b3': round(factor3, 2)
    }

# ==================== SIMULATOR CLASS ====================
class DynamicBalancingSimulator:
    def __init__(self, charge_time_minutes=30, max_cycles=3, imbalance_tolerance=5):
        self.charge_time_minutes = charge_time_minutes
        self.max_cycles = max_cycles
        self.imbalance_tolerance = imbalance_tolerance
        
        # Calculate base charge rate per update
        updates_per_cycle = (charge_time_minutes * 60) / UPDATE_INTERVAL
        self.base_charge_rate = 100.0 / updates_per_cycle
        
        # Initialize all batteries at slightly different starting percentages
        # (simulating realistic initial imbalance)
        self.batteries = {
            'b1': {
                'percentage': random.uniform(0, 5),  # 0-5% start
                'params': BATTERY_PARAMS['b1'],
                'voltage': BATTERY_PARAMS['b1']['volt_min'],
                'temp': 25.0,
                'charging': True
            },
            'b2': {
                'percentage': random.uniform(0, 5),
                'params': BATTERY_PARAMS['b2'],
                'voltage': BATTERY_PARAMS['b2']['volt_min'],
                'temp': 25.0,
                'charging': True
            },
            'b3': {
                'percentage': random.uniform(0, 5),
                'params': BATTERY_PARAMS['b3'],
                'voltage': BATTERY_PARAMS['b3']['volt_min'],
                'temp': 25.0,
                'charging': True
            }
        }
        
        self.cycle_count = 0
        self.phase = "CHARGING"  # CHARGING, DISCHARGING, COMPLETED
        self.has_sunlight = True
        
        # For tracking currents
        self.prev_currents = {'b1': 0, 'b2': 0, 'b3': 0}
        
        print(f"⚡ DYNAMIC BALANCING CONFIGURATION:")
        print(f"   - ALL batteries charge simultaneously")
        print(f"   - Balancing adjusts charging rates to keep percentages EQUAL")
        print(f"   - Base charge rate: {self.base_charge_rate:.2f}% per update")
        print(f"   - Full charge time: ~{charge_time_minutes} minutes")
        print(f"   - Imbalance tolerance: ±{imbalance_tolerance}%")
        print(f"   - Max cycles: {max_cycles}")
        print()
    
    def get_current_percentages(self):
        """Get current percentages of all batteries"""
        return {
            'b1': self.batteries['b1']['percentage'],
            'b2': self.batteries['b2']['percentage'],
            'b3': self.batteries['b3']['percentage']
        }
    
    def update_sunlight(self):
        """Update sunlight availability"""
        if self.phase == "CHARGING":
            self.has_sunlight = True
        else:
            current_hour = datetime.datetime.now().hour
            is_day = 6 <= current_hour <= 18
            self.has_sunlight = is_day and random.random() < 0.3
        return self.has_sunlight
    
    def update_all_batteries(self):
        """Update all batteries with DYNAMIC BALANCING"""
        self.update_sunlight()
        
        # Get current percentages
        percentages = self.get_current_percentages()
        
        # Calculate balancing factors based on current imbalance
        balancing_factors = calculate_balancing_factors(percentages)
        
        # Check if all batteries are fully charged
        all_charged = all(b['percentage'] >= 99.5 for b in self.batteries.values())
        
        # Handle phase transitions
        if self.phase == "CHARGING" and all_charged:
            self.cycle_count += 1
            print(f"\n🎯 Cycle {self.cycle_count} completed! All batteries reached 100%!")
            
            if self.cycle_count >= self.max_cycles:
                print(f"\n🏁 All {self.max_cycles} cycles completed!")
                self.phase = "COMPLETED"
            else:
                print(f"🔄 Starting DISCHARGE phase for cycle {self.cycle_count + 1}...")
                self.phase = "DISCHARGING"
                for battery_id in self.batteries:
                    self.batteries[battery_id]['charging'] = False
            return balancing_factors
        
        # Check if all batteries are fully discharged
        if self.phase == "DISCHARGING":
            all_empty = all(b['percentage'] <= 0.5 for b in self.batteries.values())
            if all_empty:
                print(f"\n🔄 All batteries discharged. Starting CHARGE phase for cycle {self.cycle_count + 1}...")
                self.phase = "CHARGING"
                for battery_id in self.batteries:
                    self.batteries[battery_id]['charging'] = True
            return balancing_factors
        
        # Update EACH battery with dynamic charging rates
        for battery_id, battery_data in self.batteries.items():
            params = battery_data['params']
            old_percentage = battery_data['percentage']
            balancing_factor = balancing_factors[battery_id]
            
            if self.phase == "CHARGING":
                battery_data['charging'] = True
                
                # Apply balancing factor to charge rate
                # Behind battery (factor > 1): charges FASTER
                # Ahead battery (factor < 1): charges SLOWER
                charge_rate = self.base_charge_rate * balancing_factor
                
                # Add small random variation
                charge_rate = charge_rate * random.uniform(0.95, 1.05)
                
                # Apply charge
                new_percentage = min(100.0, old_percentage + charge_rate)
                battery_data['percentage'] = new_percentage
                
                # Print balancing effect (for debugging)
                if balancing_factor != 1.0:
                    direction = "FASTER" if balancing_factor > 1 else "SLOWER"
                    print(f"     ⚡ {battery_id.upper()}: factor={balancing_factor:.2f} ({direction})")
                
            elif self.phase == "DISCHARGING":
                battery_data['charging'] = False
                discharge_rate = self.base_charge_rate * 0.4 * random.uniform(0.8, 1.2)
                new_percentage = max(0.0, old_percentage - discharge_rate)
                battery_data['percentage'] = new_percentage
            
            # Update voltage
            v_min = params['volt_min']
            v_max = params['volt_max']
            battery_data['voltage'] = percentage_to_voltage(battery_data['percentage'], v_min, v_max)
        
        return balancing_factors
    
    def generate_current(self, battery_id, battery_data, balancing_factor):
        """Generate current with balancing applied"""
        percentage = battery_data['percentage']
        params = battery_data['params']
        max_current = params['current_max']
        
        if battery_data['charging']:
            # Apply balancing factor to charging current
            current = get_charging_current(percentage, max_current, balancing_factor)
            current = current * random.uniform(0.95, 1.05)
        else:
            # Discharging - no balancing needed
            if percentage <= 10:
                current = -0.05
            elif percentage <= 20:
                current = -max_current * 0.2
            else:
                current = -random.uniform(0.3, max_current * 0.6)
            current = current * random.uniform(0.9, 1.1)
        
        current = round(current, 2)
        
        # Smooth transition
        prev = self.prev_currents[battery_id]
        if abs(current - prev) > 0.5:
            current = prev + (current - prev) * 0.3
        
        self.prev_currents[battery_id] = current
        return current
    
    def generate_battery_data(self, battery_id, ambient_temp, balancing_factor):
        """Generate complete battery data for one battery"""
        battery_data = self.batteries[battery_id]
        current = self.generate_current(battery_id, battery_data, balancing_factor)
        temp = get_temperature(
            battery_data['percentage'], 
            ambient_temp, 
            current, 
            battery_data['charging']
        )
        battery_data['temp'] = temp
        
        return {
            'voltage': battery_data['voltage'],
            'percentage': round(battery_data['percentage'], 1),
            'current': current,
            'temperature': temp
        }
    
    def generate_solar_data(self):
        """Generate solar panel currents"""
        if self.phase == "CHARGING":
            # Solar panels producing power
            solar_data = {
                'panel1_current': round(random.uniform(1.5, 2.5), 2),
                'panel2_current': round(random.uniform(2.0, 3.5), 2),
                'panel3_current': round(random.uniform(3.0, 5.5), 2)
            }
        else:
            # Night time
            solar_data = {
                'panel1_current': round(random.uniform(0, 0.1), 2),
                'panel2_current': round(random.uniform(0, 0.1), 2),
                'panel3_current': round(random.uniform(0, 0.1), 2)
            }
        return solar_data
    
    def generate_environment_data(self):
        """Generate ambient environment data"""
        current_hour = datetime.datetime.now().hour
        is_day = 6 <= current_hour <= 18
        
        if is_day and self.phase == "CHARGING":
            ldr = random.randint(2000, 4000)
            temp = random.uniform(28, 35)
        else:
            ldr = random.randint(0, 500)
            temp = random.uniform(22, 28)
        
        return {
            'ambient_temperature': round(temp, 1),
            'humidity': round(random.uniform(50, 80), 0),
            'ldr_value': ldr
        }
    
    def generate_system_data(self, battery_data_list, solar_data):
        """Generate system status data"""
        is_charging = any(data['current'] > 0.1 for data in battery_data_list)
        is_load = any(data['current'] < -0.1 for data in battery_data_list)
        
        if is_charging and is_load:
            status = 2
        elif is_charging:
            status = 1
        elif is_load:
            status = 2
        else:
            status = 0
        
        emergency = any(data['temperature'] > 50 for data in battery_data_list)
        if emergency:
            status = 3
        
        percentages = [data['percentage'] for data in battery_data_list]
        imbalance = max(percentages) - min(percentages)
        
        return {
            'status': status,
            'emergency_mode': emergency,
            'imbalance_percent': round(imbalance, 1),
            'last_update': int(time.time() * 1000)
        }
    
    def get_status_report(self):
        """Get formatted status report with imbalance info"""
        percentages = self.get_current_percentages()
        avg = sum(percentages.values()) / 3
        report = []
        for battery_id, battery_data in self.batteries.items():
            state = "🔋 CHARGING" if battery_data['charging'] else "💡 DISCHARGING"
            diff = battery_data['percentage'] - avg
            diff_str = f"({diff:+.1f}% vs avg)" if abs(diff) > 0.5 else "(balanced)"
            report.append(f"  {battery_id.upper()}: {battery_data['percentage']:.1f}% {diff_str} [{state}]")
        return "\n".join(report)

# ==================== PROGRESS BAR ====================
def print_progress_bar(percentage, battery_name, width=25):
    filled = int(width * percentage / 100)
    bar = '█' * filled + '░' * (width - filled)
    print(f"  {battery_name}: [{bar}] {percentage:.1f}%", end='')

# ==================== FIREBASE WRITE FUNCTIONS ====================
def send_current_data(ref, simulator):
    # First update all batteries with dynamic balancing
    balancing_factors = simulator.update_all_batteries()
    
    # Generate environment data
    env_data = simulator.generate_environment_data()
    ambient_temp = env_data['ambient_temperature']
    
    # Generate battery data for all 3 batteries with balancing factors
    b1_data = simulator.generate_battery_data('b1', ambient_temp, balancing_factors['b1'])
    b2_data = simulator.generate_battery_data('b2', ambient_temp, balancing_factors['b2'])
    b3_data = simulator.generate_battery_data('b3', ambient_temp, balancing_factors['b3'])
    battery_data_list = [b1_data, b2_data, b3_data]
    
    # Generate solar and system data
    solar_data = simulator.generate_solar_data()
    system_data = simulator.generate_system_data(battery_data_list, solar_data)
    
    # Write to Firebase
    print("\n📤 Sending to Firebase...")
    print(f"📊 Phase: {simulator.phase}")
    
    # Battery data
    ref.child('current/battery1').set(b1_data)
    charge_symbol = "🔋" if b1_data['current'] > 0 else "💡"
    print(f"  {charge_symbol} Battery 1: {b1_data['voltage']}V, {b1_data['percentage']}%, {b1_data['current']}A, {b1_data['temperature']}°C (factor: {balancing_factors['b1']:.2f})")
    
    ref.child('current/battery2').set(b2_data)
    charge_symbol = "🔋" if b2_data['current'] > 0 else "💡"
    print(f"  {charge_symbol} Battery 2: {b2_data['voltage']}V, {b2_data['percentage']}%, {b2_data['current']}A, {b2_data['temperature']}°C (factor: {balancing_factors['b2']:.2f})")
    
    ref.child('current/battery3').set(b3_data)
    charge_symbol = "🔋" if b3_data['current'] > 0 else "💡"
    print(f"  {charge_symbol} Battery 3: {b3_data['voltage']}V, {b3_data['percentage']}%, {b3_data['current']}A, {b3_data['temperature']}°C (factor: {balancing_factors['b3']:.2f})")
    
    # Solar data
    ref.child('current/solar').set(solar_data)
    print(f"  ☀️ Solar: {solar_data['panel1_current']}A, {solar_data['panel2_current']}A, {solar_data['panel3_current']}A")
    
    # System data
    ref.child('current/system').set(system_data)
    status_names = {0: 'Idle', 1: 'Charging', 2: 'Load Active', 3: 'ALERT'}
    print(f"  📊 System: {status_names[system_data['status']]}, Imbalance: {system_data['imbalance_percent']}%")
    
    # Environment data
    ref.child('current/environment').set(env_data)
    print(f"  🌡️ Environment: {env_data['ambient_temperature']}°C, {env_data['humidity']}%, LDR: {env_data['ldr_value']}")
    
    return {
        'b1': b1_data,
        'b2': b2_data,
        'b3': b3_data,
        'solar': solar_data,
        'system': system_data,
        'environment': env_data
    }

def save_historical_data(ref, current_data):
    timestamp = int(time.time() * 1000)
    day_folder = datetime.datetime.now().strftime('%Y-%m-%d')
    history_path = f'history/{day_folder}/{timestamp}'
    
    print(f"\n📦 Saving historical data to: {history_path}")
    ref.child(f'{history_path}/battery1').set(current_data['b1'])
    ref.child(f'{history_path}/battery2').set(current_data['b2'])
    ref.child(f'{history_path}/battery3').set(current_data['b3'])
    ref.child(f'{history_path}/system').set({
        'status': current_data['system']['status'],
        'imbalance': current_data['system']['imbalance_percent']
    })
    print("✓ Historical data saved")

def signal_handler(sig, frame):
    print("\n\n🛑 Seeder stopped by user")
    sys.exit(0)

def main():
    parser = argparse.ArgumentParser(description='Dynamic Balancing Battery Charger Simulator')
    parser.add_argument('--once', action='store_true', help='Run once and exit')
    parser.add_argument('--interval', type=int, default=UPDATE_INTERVAL, help=f'Update interval in seconds (default: {UPDATE_INTERVAL})')
    parser.add_argument('--no-history', action='store_true', help='Disable historical data saving')
    parser.add_argument('--charge-time', type=int, default=DEFAULT_CHARGE_TIME_MINUTES, help=f'Time to full charge (default: {DEFAULT_CHARGE_TIME_MINUTES})')
    parser.add_argument('--cycles', type=int, default=DEFAULT_MAX_CHARGE_CYCLES, help=f'Number of cycles (default: {DEFAULT_MAX_CHARGE_CYCLES})')
    parser.add_argument('--tolerance', type=int, default=DEFAULT_IMBALANCE_TOLERANCE, help=f'Imbalance tolerance % (default: {DEFAULT_IMBALANCE_TOLERANCE})')
    args = parser.parse_args()
    
    signal.signal(signal.SIGINT, signal_handler)
    
    print("=" * 70)
    print("🔋 DYNAMIC BALANCING BATTERY CHARGER SIMULATION")
    print("=" * 70)
    print(f"📊 Goal: Maintain EQUAL percentage across all 3 batteries")
    print(f"⚡ ALL batteries charge simultaneously but at ADJUSTABLE rates")
    print(f"   - Battery with LOWER percentage charges FASTER")
    print(f"   - Battery with HIGHER percentage charges SLOWER")
    print(f"   - Result: All batteries stay balanced!")
    print(f"\n📋 Settings:")
    print(f"   - Full charge time: ~{args.charge_time} minutes")
    print(f"   - Max cycles: {args.cycles}")
    print(f"   - Update interval: {args.interval} seconds")
    print("=" * 70)
    
    ref = initialize_firebase()
    simulator = DynamicBalancingSimulator(
        charge_time_minutes=args.charge_time,
        max_cycles=args.cycles,
        imbalance_tolerance=args.tolerance
    )
    
    last_history_save = time.time()
    start_time = time.time()
    
    print("\n🚀 Starting DYNAMIC BALANCING simulation...")
    print("   Batteries will automatically adjust charging rates to stay EQUAL")
    print("   Press Ctrl+C to stop\n")
    
    print("Initial battery status (slight imbalance simulated):")
    print(simulator.get_status_report())
    print()
    
    try:
        while True:
            current_data = send_current_data(ref, simulator)
            
            print("\n📊 Current charge levels with balancing:")
            percentages = simulator.get_current_percentages()
            avg = sum(percentages.values()) / 3
            for i, battery_id in enumerate(['b1', 'b2', 'b3'], 1):
                batt = simulator.batteries[battery_id]
                diff = batt['percentage'] - avg
                marker = "✓" if abs(diff) < 2 else ("↑" if diff > 0 else "↓")
                print_progress_bar(batt['percentage'], f"Battery {i}")
                state = "CHARGING" if batt['charging'] else "DISCHARGING"
                print(f" {state} {marker} ({diff:+.1f}% vs avg)")
            
            elapsed = time.time() - start_time
            print(f"\n⏱️ Elapsed: {int(elapsed // 60)}m {int(elapsed % 60)}s")
            print(f"📊 Average charge: {avg:.1f}%")
            
            if simulator.phase == "COMPLETED":
                print(f"\n✅ Simulation completed: {simulator.max_cycles} cycles finished!")
                if args.once:
                    break
            
            if not args.no_history and ENABLE_HISTORY:
                current_time = time.time()
                if current_time - last_history_save >= HISTORY_INTERVAL:
                    save_historical_data(ref, current_data)
                    last_history_save = current_time
            
            if args.once:
                print("\n✅ Seeder completed (--once mode)")
                break
            
            time.sleep(args.interval)
            
    except KeyboardInterrupt:
        print("\n\n🛑 Seeder stopped by user")
        print("\n📈 Final battery status:")
        print(simulator.get_status_report())
    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()