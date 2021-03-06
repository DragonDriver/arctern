FROM python:3.7-alpine

COPY gui/server/tests/restful/requirements.txt /requirements.txt

RUN pip3 install --no-cache-dir \
    -r /requirements.txt