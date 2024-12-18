import firebase_admin
from firebase_admin import credentials, firestore
import time

# Path to the service account key JSON file
cred = credentials.Certificate('REPLACE WITH FIREBASE JSON CREDENTIAL')

# Initialize the app with the service account
firebase_admin.initialize_app(cred)

# Get a Firestore client
db = firestore.client()

def process_device_data_and_update_users():
    # Reference to the 'device' collection
    device_collection_ref = db.collection('device')

    # Fetch all documents in the 'device' collection
    docs = device_collection_ref.stream()

    # Dictionary to store the sum of fields for each user
    user_data = {}

    # Set to track users who own devices
    users_with_devices = set()

    # Iterate through the documents in the 'device' collection
    for doc in docs:
        doc_data = doc.to_dict()

        # Check if 'owner' and relevant fields exist
        if 'owner' in doc_data:
            owner_ref = doc_data['owner']  # This is a reference to the user
            peso = doc_data.get('pesoState', 0.0)

            # Ensure values are of type double (float in Python)
            peso = float(peso)

            # Convert the owner reference to its document ID
            owner_id = owner_ref.id

            # Add this user to the set of users with devices
            users_with_devices.add(owner_id)

            # Sum the values for each user
            if owner_id in user_data:
                user_data[owner_id]['pesoSum'] += peso
            else:
                user_data[owner_id] = {
                    'pesoSum': peso
                }

    # Now update the users collection with the summed data
    for user_id, data in user_data.items():
        # Reference to the specific user document in the 'users' collection
        user_ref = db.collection('users').document(user_id)
        
        # Update or create the fields with the calculated sums
        user_ref.set({
            'pesoSum': data['pesoSum']
        }, merge=True)

    # Check for users who have no devices left
    users_ref = db.collection('users')
    all_users = users_ref.stream()

    for user in all_users:
        user_id = user.id
        if user_id not in users_with_devices:
            # Reset sums to zero if the user has no devices
            user_ref = db.collection('users').document(user_id)
            user_ref.set({
                'pesoSum': 0.0
            }, merge=True)

    print("Data sums have been updated in the 'users' collection.")

if __name__ == "__main__":
    while True:
        process_device_data_and_update_users()
        time.sleep(20)  
