# To run server, assuming you have already cloned the "makerlab-remote-control" repo
- Install git, python (v3.13.12). nodejs (LTS v24.14.1)

cd `~\GitHub\makerlab-remote-control>`

## If an existing .venv folder exists, and it's your first time:
- Ensure your not in venv currently, and if so, `deactivate`
- `Remove-Item -Recurse -Force .venv`
- Then `py -3.13 -m venv .venv` to create the environment

## Start the enviroment
- Activate venv `.\.venv\Scripts\Activate`
- If first time, `pip install -r server/requirements.txt`
- If first time, `Ctrl+Shift+P`, `Python: Select Interpreter`, and choose correct venv (you need Python extension for VSCode)

## Run uvivorn
- Open a new PowerShell terminal.
- Run `uvicorn app.main:app --app-dir server --reload --host 0.0.0.0 --port 8000`

## Run front end (npm)
- Open new Powershell terminal
- `cd frontend`
- `npm install`
- `npm run dev`

Go to '