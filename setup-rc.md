# To run server, assuming you have already cloned the "makerlab-remote-control" repo

cd `~\GitHub\makerlab-remote-control>`

First time on PC, then `py -m venv .venv` to create the environment
Activate venv `.\.venv\Scripts\Activate`
First time on PC, then `pip install -r server/requirements.txt`

Open a new PowerShell terminal.
Run `uvicorn app.main:app --app-dir server --reload --host 0.0.0.0 --port 8000`

cd `frontend` and `npm install`. Then `npm run dev`