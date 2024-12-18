from datetime import datetime, timedelta
import firebase_admin
from firebase_admin import credentials, firestore
import time
import logging
from typing import Dict, List, Optional
from dataclasses import dataclass
from functools import wraps
from concurrent.futures import ThreadPoolExecutor

# Set up logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

@dataclass
class DeviceData:
    id: str
    owner_id: str
    peso_state: float
    days: List[float]
    day_name: List[str]
    month: List[float]
    month_name: List[str]
    monthly_reset_day: Optional[int]

@dataclass
class UserData:
    id: str
    days: List[float]
    day_name: List[str]
    month: List[float]
    month_name: List[str]
    days_sum: float = 0.0
    month_sum: float = 0.0

class FirebaseManager:
    def __init__(self, credentials_path: str):
        try:
            cred = credentials.Certificate(credentials_path)
            firebase_admin.initialize_app(cred)
            self.db = firestore.client()
            logger.info("Firebase connection established successfully")
        except Exception as e:
            logger.error(f"Failed to initialize Firebase: {e}")
            raise

    def retry_on_failure(max_retries: int = 3, delay: float = 1.0):
        def decorator(func):
            @wraps(func)
            def wrapper(*args, **kwargs):
                last_error = None
                for attempt in range(max_retries):
                    try:
                        return func(*args, **kwargs)
                    except Exception as e:
                        last_error = e
                        logger.warning(f"Attempt {attempt + 1} failed: {e}")
                        if attempt < max_retries - 1:
                            time.sleep(delay)
                logger.error(f"Operation failed after {max_retries} attempts: {last_error}")
                raise last_error
            return wrapper
        return decorator

    @retry_on_failure()
    def update_device_data(self, device: DeviceData, updates: dict) -> None:
        device_ref = self.db.collection('device').document(device.id)
        device_ref.update(updates)
        if logger.isEnabledFor(logging.INFO):
            logger.info(f"Updated device {device.id} with data: {updates}")

    @retry_on_failure()
    def update_user_data(self, user: UserData, updates: dict) -> None:
        user_ref = self.db.collection('users').document(user.id)
        user_ref.update(updates)
        if logger.isEnabledFor(logging.INFO):
            logger.info(f"Updated user {user.id} with data: {updates}")

    def process_daily_updates(self) -> None:
        try:
            now = datetime.now()
            current_month = now.strftime('%b')
            devices = self._fetch_device_data()
            users = self._fetch_user_data()

            with ThreadPoolExecutor() as executor:
                # Process devices
                for device in devices:
                    executor.submit(self.process_device_update, device, users, now, current_month)

                # Update users
                for user in users.values():
                    executor.submit(self.process_user_update, user, now.day, current_month)

            if logger.isEnabledFor(logging.INFO):
                logger.info(f"Daily update completed successfully at {now}")

        except Exception as e:
            logger.error(f"Failed to process daily updates: {e}")
            raise

    def process_device_update(self, device: DeviceData, users: Dict[str, UserData], now: datetime, current_month: str):
        user = users.get(device.owner_id)
        if not user:
            logger.warning(f"No user found for device {device.id}")
            return

        updates = {}
        is_reset_day = self.should_process_monthly_reset(now, device)
        if is_reset_day:
            monthly_sum = sum(device.days)
            updates = {
                'month': firestore.ArrayUnion([monthly_sum]),
                'monthName': firestore.ArrayUnion([current_month]),
                'days': [],
                'dayName': []
            }
            user.month_sum += monthly_sum
            logger.info(f"Processed monthly reset for device {device.id} with sum {monthly_sum}")
        else:
            updates = {
                'days': firestore.ArrayUnion([device.peso_state]),
                'dayName': firestore.ArrayUnion([str(now.day)])
            }
            user.days_sum += device.peso_state

        self.update_device_data(device, updates)

    def process_user_update(self, user: UserData, today_day: int, current_month: str):
        updates = {
            'days': firestore.ArrayUnion([user.days_sum]),
            'dayName': firestore.ArrayUnion([str(today_day)])
        }
        if self.should_process_monthly_reset(datetime.now(), user):
            updates.update({
                'month': firestore.ArrayUnion([user.month_sum]),
                'monthName': firestore.ArrayUnion([current_month])
            })
        self.update_user_data(user, updates)

    def should_process_monthly_reset(self, now: datetime, device: DeviceData) -> bool:
        yesterday = now - timedelta(days=1)
        return yesterday.day == device.monthly_reset_day

    def _fetch_user_data(self) -> Dict[str, UserData]:
        users = {}
        for doc in self.db.collection('users').stream():
            data = doc.to_dict()
            users[doc.id] = UserData(
                id=doc.id,
                days=data.get('days', []),
                day_name=data.get('dayName', []),
                month=data.get('month', []),
                month_name=data.get('monthName', []),
                days_sum=0.0,  # Initialize as 0.0 for calculations
                month_sum=0.0  # Initialize as 0.0 for calculations
            )
        return users

    def _fetch_device_data(self) -> List[DeviceData]:
        devices = []
        for doc in self.db.collection('device').stream():
            data = doc.to_dict()
            if 'owner' not in data:
                continue

            # Convert monthlyReset to an integer if it exists, or set it to None
            monthly_reset_day = None
            if 'monthlyReset' in data:
                try:
                    monthly_reset_day = int(data['monthlyReset'])
                except ValueError:
                    logger.warning(f"Invalid 'monthlyReset' value for device {doc.id}: {data['monthlyReset']}")
                    monthly_reset_day = None  # Default to None if conversion fails

            devices.append(DeviceData(
                id=doc.id,
                owner_id=data['owner'].id,
                peso_state=data.get('pesoState', 0.0),
                days=data.get('days', []),
                day_name=data.get('dayName', []),
                month=data.get('month', []),
                month_name=data.get('monthName', []),
                monthly_reset_day=monthly_reset_day  # Use converted value or None
            ))
        return devices

    def run_scheduler(self):
        last_execution_date = None
        next_run_time = datetime.now().replace(hour=10, minute=58, second=30, microsecond=0)
        while True:
            try:
                current_time = datetime.now()
                if current_time >= next_run_time:
                    if last_execution_date is None or current_time.date() > last_execution_date:
                        logger.info(f"Starting scheduled execution at {current_time}")
                        self.process_daily_updates()
                        last_execution_date = current_time.date()
                    next_run_time += timedelta(days=1)
                time.sleep(0.25)
            except Exception as e:
                logger.error(f"Scheduler iteration failed: {e}")
                time.sleep(1)

def main():
    try:
        firebase_manager = FirebaseManager('REPLACE WITH FIREBASE JSON CREDENTIAL')
        firebase_manager.run_scheduler()
    except Exception as e:
        logger.critical(f"Application failed to start: {e}")
        raise

if __name__ == "__main__":
    main()