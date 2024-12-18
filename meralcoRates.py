import requests
from bs4 import BeautifulSoup
import re
import firebase_admin
from firebase_admin import credentials, firestore
import time
from datetime import datetime, timedelta

# Initialize Firestore using a service account key
cred = credentials.Certificate('REPLACE WITH FIREBASE JSON CREDENTIAL')
firebase_admin.initialize_app(cred)

# Initialize Firestore DB
db = firestore.client()

def get_latest_article_url():
    url = "https://company.meralco.com.ph/news-and-advisories"
    
    response = requests.get(url)
    soup = BeautifulSoup(response.content, 'html.parser')
    
    # Find the first article link with the correct class
    article = soup.find('a', class_='btn btn-default', href=True)
    
    if article:
        article_url = article['href']
        if article_url.startswith('http'):
            return article_url
        else:
            return "https://company.meralco.com.ph" + article_url
    return None

def extract_overall_rate(article_url):
    response = requests.get(article_url)
    
    if response.status_code == 200:
        soup = BeautifulSoup(response.content, 'html.parser')
        text = soup.get_text()

        # Search for the overall rate pattern in the article
        pattern = r"overall rate .*?P([\d.]+)"
        match = re.search(pattern, text, re.IGNORECASE | re.DOTALL)

        if match:
            overall_rate = float(match.group(1))  # Convert to float
            return overall_rate
    return None

def get_previous_rate():
    # Retrieve the previous rate from Firestore
    doc_ref = db.collection('meralcoConversion').document('currentConversion')
    doc = doc_ref.get()
    
    if doc.exists:
        return doc.to_dict().get('kWhToPeso')
    return None

def update_firestore(overall_rate):
    doc_ref = db.collection('meralcoConversion').document('currentConversion')
    data = {'kWhToPeso': overall_rate}
    doc_ref.set(data)
    print(f"Data successfully written to Firestore: {data}")

def monitor_website():
    latest_article_url = get_latest_article_url()

    if latest_article_url:
        overall_rate = extract_overall_rate(latest_article_url)
        
        if overall_rate is not None:
            previous_rate = get_previous_rate()
            
            # If the rate has changed, update Firestore
            if previous_rate is None or overall_rate != previous_rate:
                print(f"New Overall Rate: {overall_rate}. Updating Firestore...")
                update_firestore(overall_rate)
            else:
                print("No change in the overall rate.")
        else:
            print("Couldn't extract the overall rate.")
    else:
        print("Couldn't find the latest article.")

def run_without_sleep():
    last_execution_date = None

    while True:
        current_time = datetime.now()
        current_date = current_time.date()

        # Check if it's midnight and we haven't run the process yet today
        if current_time.hour == 00 and current_time.minute == 00 and current_time.second == 00 and (last_execution_date is None or current_date > last_execution_date):
            print(f"Executing at midnight: {current_time.strftime('%Y-%m-%d %H:%M:%S')}")

        # Perform the rate extraction and updating Firestore
        current_rate = extract_overall_rate(get_latest_article_url())
        previous_rate = get_previous_rate()
        
        if current_rate and (previous_rate is None or current_rate != previous_rate):
            # If the rate has changed or there's no previous rate, update Firestore
            print(f"Detected new rate: {current_rate}, updating Firestore.")
            update_firestore(current_rate)
        else:
            # No changes detected
            print("Overall rate is unchanged, no action needed.")

            # Update the last execution date
            last_execution_date = current_date

        # Briefly pause to prevent high CPU usage
        time.sleep(0.25)  # Check once per second

# Start monitoring
if __name__ == "__main__":
    while True:
        # Sleep until the next midnight
        run_without_sleep()

