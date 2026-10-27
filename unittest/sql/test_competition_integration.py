#!/usr/bin/env python3
"""SQL integration checks. Requires pymysql and the local mock_ai_service.py."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import urllib.request
import uuid
import pymysql


def run(port, fixture_parent, mock_port):
    connection = pymysql.connect(host='127.0.0.1', port=port, user='root',
                                 database='test', autocommit=True, charset='utf8mb4')
    cursor = connection.cursor()
    passed = 0

    def sql(query, args=None):
        cursor.execute(query, args)
        return cursor.fetchall()

    def check(condition, message):
        nonlocal passed
        assert condition, message
        passed += 1
        print('PASS', message, flush=True)

    def fails(query, args=None):
        try:
            sql(query, args)
        except pymysql.MySQLError:
            return True
        return False

    prefix = 'lab_' + uuid.uuid4().hex[:10]
    models = []

    def model(suffix, kind, provider, endpoint):
        name = prefix + suffix
        sql('call dbms_ai_service.create_ai_model(%s,%s)',
            (name, json.dumps({'type': kind, 'model_name': name})))
        models.append(name)
        sql('call dbms_ai_service.create_ai_model_endpoint(%s,%s)',
            (name + '_ep', json.dumps({'ai_model_name': name,
             'url': f'http://127.0.0.1:{mock_port}{endpoint}',
             'access_key': '' if provider.lower() == 'ollama' else 'fixture-key',
             'provider': provider})))
        return name

    def stats():
        return json.load(urllib.request.urlopen(f'http://127.0.0.1:{mock_port}/stats'))

    try:
        with tempfile.TemporaryDirectory(prefix='file-sql-', dir=fixture_parent) as directory:
            root = Path(directory)
            csv = root / 'input.csv'
            csv.write_text('id,name,score\n1,"你好",1.5\n2,"a,b",2\n3,,3.5\n')
            ndjson = root / 'input.jsonl'
            ndjson.write_text('{"id":1,"tag":"one"}\n{"id":3,"tag":"three","extra":true}\n')
            sql('set enable_file_sql=off')
            check(fails('select * from read_csv(%s)', (str(csv),)), 'file SQL is disabled by default')
            sql('set enable_file_sql=on')
            check(sql('select count(*),sum(id),sum(score) from read_csv(%s)', (str(csv),)) ==
                  ((3, 6, 7.0),), 'native CSV aggregates')
            check(sql('select name from read_csv(%s) where id=2', (str(csv),)) ==
                  (('a,b',),), 'CSV quoted delimiter')
            check(sql('select id from read_csv(%s) where score>1 order by score desc limit 2',
                      (str(csv),)) == ((3,), (2,)), 'filter order and limit')
            check(sql('select a.id,b.tag from read_csv(%s) a left join read_ndjson(%s) b on a.id=b.id order by a.id',
                      (str(csv), str(ndjson))) == ((1, 'one'), (2, None), (3, 'three')),
                  'native cross-format LEFT JOIN')
            check(sql('select a.id from read_csv(%s) a inner join read_json(%s) b on a.id=b.id order by a.id',
                      (str(csv), str(ndjson))) == ((1,), (3,)), 'native INNER JOIN')
            description = sql('describe select * from read_csv(%s)', (str(csv),))
            check([row[:2] for row in description] == [('id', 'BIGINT'), ('name', 'VARCHAR'), ('score', 'DOUBLE')],
                  'dynamic DESCRIBE columns')
            output = root / 'output.csv'
            sql("copy (select * from read_csv(%s) order by id) to %s (format csv, header, nullstr 'NULL')",
                (str(csv), str(output)))
            check(sql("select * from read_csv(%s, nullstr='NULL') order by id", (str(output),)) ==
                  sql('select * from read_csv(%s) order by id', (str(csv),)), 'COPY round trip')
            check(fails('copy (select 1) to %s', (str(output),)), 'COPY refuses an existing target')
            failed_output = root / 'failure.csv'
            check(fails("copy (select json_extract(case when id=2 then 'bad' else '{\"k\":1}' end,'$.k') from read_csv(%s)) to %s",
                        (str(csv), str(failed_output))), 'COPY reports evaluation errors')
            check(not failed_output.exists() and not list(root.glob('.seekdb-copy-*')), 'COPY cleans temporary files')
            discovered = sql('select file from glob(%s) order by file', (str(root / '*'),))
            check([row[0] for row in discovered] == sorted(str(p) for p in root.iterdir()), 'glob discovery')
            for name in ['nullable-v1.parquet', 'nullable-v2.parquet']:
                parquet = Path(__file__).parent / 'engine/basic/data' / name
                check(sql('select count(*),sum(id),sum(active) from read_parquet(%s)', (str(parquet.resolve()),)) ==
                      ((5, 15, 2),), 'Parquet multi-group nullable ' + name)
            check(fails('select * from read_csv(%s,sample_size=10)', (str(csv),)), 'sample_size rejects sampling')
            copy_table = prefix + '_copy'
            sql('create table ' + copy_table + '(id int, content longtext)')
            try:
                long_text = '长文本' * 10000 + ',"\nend'
                sql('insert into ' + copy_table + ' values(1,%s),(2,null)', (long_text,))
                table_output = root / 'table.csv'
                sql("copy " + copy_table + " to %s (format csv, header, delimiter ';', nullstr 'NULL')", (str(table_output),))
                check(sql("select id,content from read_csv(%s, delimiter=';', nullstr='NULL') order by id", (str(table_output),)) ==
                      ((1, long_text), (2, None)), 'COPY table and out-of-row LONGTEXT with custom delimiter')
            finally:
                sql('drop table ' + copy_table)

            check(sql('select name,count(*),sum(id) from read_csv(%s) group by name having sum(id)>1 order by sum(id)', (str(csv),)) == (('a,b', 1, 2), (None, 1, 3)), 'native GROUP BY and HAVING')

        complete = model('_c', 'completion', 'openai', '/v1/chat/completions')
        embed = model('_e', 'dense_embedding', 'openai', '/v1/embeddings')
        rerank = model('_r', 'rerank', 'siliconflow', '/v1/rerank')
        ollama = model('_o', 'completion', 'oLlAmA', '/api/generate')
        ollama_embed = model('_oe', 'dense_embedding', 'OLLAMA', '/api/embed')
        check(sql('select ai_complete(%s,%s,null)', (complete, '你好')) == (('你好',),), 'Completion and NULL config')
        check(sql("select ai_complete(%s,ai_prompt('{0}/{0}',json_object('k',1)),json_object('temperature',0.2,'max_tokens',16))",
                  (complete,))[0][0] == '{"k": 1}/{"k": 1}', 'JSON Prompt expansion and config')
        sql('call dbms_ai_service.alter_ai_model_endpoint(%s,%s)', (ollama + '_ep', json.dumps({'provider': 'Ollama'})))
        check(sql('select ai_complete(%s,%s)', (ollama, 'native generation')) == (('native generation',),), 'Ollama native generation')
        check(sql('select json_length(ai_embed(%s,%s,4))', (ollama_embed, 'native embedding')) == ((4,),), 'Ollama native embedding')
        table = prefix + '_batch'
        sql('create table ' + table + '(id int primary key,content text,dim int)')
        try:
            cursor.executemany('insert into ' + table + ' values(%s,%s,%s)',
                               [(i, None if i % 13 == 0 else f'text-{i}', 3 + i % 2) for i in range(137)])
            before = len(stats())
            rows = sql('select id,content,dim,ai_embed(%s,content,dim) from ' + table + ' where id>2 order by id limit 130', (embed,))
            for row_id, content, dimension, value in rows:
                if content is None:
                    assert value is None
                else:
                    digest = hashlib.sha256(content.encode()).digest()
                    expected = [round((digest[i % 32] - 128) / 128, 6) for i in range(dimension)]
                    assert json.loads(value) == expected, (row_id, value, expected)
            sent = stats()[before:]
            check(any(len(request['body'].get('input', [])) > 1 for request in sent), 'Embedding batches contain multiple SQL rows')
            check(len(rows) == 130, 'Embedding NULL/filter/LIMIT and varying dimensions retain row mapping')
        finally:
            sql('drop table ' + table)
        docs = [{'id': i, 'text': 'apple' * (i % 3)} for i in range(47)]
        result = json.loads(sql('select ai_rerank(%s,%s,%s,%s)', (rerank, 'apple', json.dumps(docs), 'text'))[0][0])
        check([item['id'] for item in result] == sorted(range(47), key=lambda i: (-(i % 3), i)), 'Rerank batches use global indices and stable ties')
        check(fails('select ai_embed(%s,%s,0)', (embed, 'invalid dimension')), 'Embedding rejects a zero dimension')
        for kind, endpoint, query, provider in [
            ('completion', '/invalid/completion', "select ai_complete(%s,'x')", 'openai'),
            ('dense_embedding', '/invalid/embedding', "select ai_embed(%s,'x')", 'openai'),
            ('rerank', '/invalid/rerank', "select ai_rerank(%s,'x',json_array('x'))", 'siliconflow')]:
            invalid = model('_bad_' + kind, kind, provider, endpoint)
            check(fails(query, (invalid,)), 'invalid service response is rejected: ' + kind)
        check(sql('select 1') == ((1,),), 'ordinary SQL remains usable')
        print(f'{passed} integration checks passed', flush=True)
    finally:
        for name in reversed(models):
            for operation, identifier in [('drop_ai_model_endpoint', name + '_ep'), ('drop_ai_model', name)]:
                try:
                    sql('call dbms_ai_service.' + operation + '(%s)', (identifier,))
                except pymysql.MySQLError:
                    pass
        cursor.close()
        connection.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--fixture-parent', type=Path, required=True)
    parser.add_argument('--mock-port', type=int, default=8000)
    args = parser.parse_args()
    run(args.port, args.fixture_parent, args.mock_port)
