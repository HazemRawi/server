if ($basic_stage == create_table)
{
# Create FTS table
--error ER_NO_INDEX_ON_TEMPORARY
CREATE TEMPORARY TABLE articles (
	id INT UNSIGNED AUTO_INCREMENT NOT NULL PRIMARY KEY,
	title VARCHAR(200),
	body TEXT,
	FULLTEXT (title,body)
	) ENGINE=InnoDB;

--disable_query_log
eval CREATE TABLE articles (
	id INT UNSIGNED AUTO_INCREMENT NOT NULL PRIMARY KEY,
	title VARCHAR(200),
	body TEXT,
	FULLTEXT (title,body)
	)$create_options ENGINE=InnoDB;
--enable_query_log
}

if ($basic_stage == insert_1)
{
# Insert six rows
INSERT INTO articles (title,body) VALUES
	('MySQL Tutorial','DBMS stands for DataBase ...')  ,
	('How To Use MySQL Well','After you went through a ...'),
	('Optimizing MySQL','In this tutorial we will show ...'),
	('1001 MySQL Tricks','1. Never run mysqld as root. 2. ...'),
	('MySQL vs. YourSQL','In the following database comparison ...'),
	('MySQL Security','When configured properly, MySQL ...');
}

if ($basic_stage == select_1)
{
# Look for 'Database' in table article
SELECT * FROM articles
	WHERE MATCH (title,body)
	AGAINST ('Database' IN NATURAL LANGUAGE MODE);

SELECT COUNT(*) FROM articles
	WHERE MATCH (title,body)
	AGAINST ('database' IN NATURAL LANGUAGE MODE);

SELECT * FROM articles
     WHERE MATCH (title, body)
     AGAINST ('Tutorial' IN NATURAL LANGUAGE MODE);


SELECT COUNT(IF(MATCH (title,body)
	AGAINST ('database' IN NATURAL LANGUAGE MODE), 1, NULL))
	AS count FROM articles;

# Select Relevance Ranking
SELECT id,  body, MATCH (title,body)
	AGAINST ('Database' IN NATURAL LANGUAGE MODE) AS score
	FROM articles;

# 'MySQL' treated as stopword (stopword functionality not yet supported)
SELECT * FROM articles
	WHERE MATCH (title,body)
	AGAINST ('MySQL' IN NATURAL LANGUAGE MODE);

# Boolean search
# Select rows contain "MySQL" but not "YourSQL"
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('+MySQL -YourSQL' IN BOOLEAN MODE);

# Select rows contain at least one of the two words
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('DBMS Security' IN BOOLEAN MODE);

# Select rows contain both "MySQL" and "YourSQL"
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('+MySQL +YourSQL' IN BOOLEAN MODE);

# Select rows contain "MySQL" but rank rows with "YourSQL" higher
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('+MySQL YourSQL' IN BOOLEAN MODE);

# Test negation operator. Select rows contain MySQL,
# if the row contains "YourSQL", rank it lower
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('+MySQL ~YourSQL' IN BOOLEAN MODE);

# Test wild card search operator
# Notice row with "the" will not get fetched due to
# stopword filtering
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('t*' IN BOOLEAN MODE);

# Test wild card search, notice row 6 with 2 "MySQL" rank first
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('MY*' IN BOOLEAN MODE);

# Another wild card search
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('ru*' IN BOOLEAN MODE);

# Test ">" and "<" Operator, the ">" operator increases
# the word relevance rank and the "<" operator decreases it
# Following test puts rows with "Well" on top and rows
# with "stands" at the bottom
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('+ MySQL >Well < stands' IN BOOLEAN MODE);

# Test sub-expression boolean search. Find rows contain
# "MySQL" but not "Well" or "stands".
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('+ MySQL - (Well stands)' IN BOOLEAN MODE);

--error 128
SELECT * FROM articles WHERE MATCH (title,body) AGAINST
('(((((((((((((((((((((((((((((((((Security)))))))))))))))))))))))))))))))))'
 IN BOOLEAN MODE);
SELECT * FROM articles WHERE MATCH (title,body) AGAINST
('((((((((((((((((((((((((((((((((Security))))))))))))))))))))))))))))))))'
 IN BOOLEAN MODE);
SELECT * FROM articles WHERE MATCH (title,body) AGAINST
('(((((((((((((((((((((((((((((((vs))))))))))))))))))))))))))))))),(((to)))'
 IN BOOLEAN MODE);

--error ER_PARSE_ERROR
SELECT * FROM articles WHERE MATCH (title,body) AGAINST
('((((((((((((((((((((((((((((((((Security)))))))))))))))))))))))))))))))'
 IN BOOLEAN MODE);
--error ER_PARSE_ERROR
SELECT * FROM articles WHERE MATCH (title,body) AGAINST
('(((((((((((((((((((((((((((((((((Security))))))))))))))))))))))))))))))))'
 IN BOOLEAN MODE);

# Test sub-expression boolean search. Find rows contain
# "MySQL" and "Well" or "MySQL" and "stands". But rank the
# doc with "Well" higher, and doc with "stands" lower.
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('+ MySQL + (>Well < stands)' IN BOOLEAN MODE);

# Test nested sub-expression.
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('YourSQL + (+MySQL - (Tricks Security))' IN BOOLEAN MODE);

# Find rows with "MySQL" but not "Tricks", "Security" nor "YourSQL"
SELECT * FROM articles WHERE MATCH (title,body)
	AGAINST ('(+MySQL - (Tricks Security)) - YourSQL' IN BOOLEAN MODE);

# Test non-word delimiter combined with negate "-" operator
# This should return the same result as 'mysql - (Security DBMS)'
SELECT * FROM articles WHERE MATCH (title,body) AGAINST ('mysql - Security&DBMS' IN BOOLEAN MODE);
SELECT * FROM articles WHERE MATCH (title,body) AGAINST ('mysql - (Security DBMS)' IN BOOLEAN MODE);

# Again, the operator sequence should not matter
SELECT * FROM articles WHERE MATCH (title,body) AGAINST (' - Security&DBMS + YourSQL' IN BOOLEAN MODE);

SELECT * FROM articles WHERE MATCH (title,body) AGAINST ('+YourSQL - Security&DBMS' IN BOOLEAN MODE);

# Test query expansion
SELECT COUNT(*) FROM articles
        WHERE MATCH (title,body)
        AGAINST ('database' WITH QUERY EXPANSION);
}

if ($basic_stage == insert_2)
{
INSERT INTO articles (title,body) VALUES
	('test query expansion','for database ...');
}

if ($basic_stage == select_2)
{
# This query will return result containing word "database" as
# the query expand from "test" to words in document just
# inserted above
SELECT * FROM articles
        WHERE MATCH (title,body)
        AGAINST ('test' WITH QUERY EXPANSION);

# This is to test the proximity search, search two word
# "following" and "comparison" within 19 character space
SELECT * FROM articles
        WHERE MATCH (title,body)
        AGAINST ('"following comparison"@3' IN BOOLEAN MODE);

# This is to test the proximity search, search two word
# "following" and "comparison" within 19 character space
# This search should come with no return result
SELECT * FROM articles
        WHERE MATCH (title,body)
        AGAINST ('"following comparison"@2' IN BOOLEAN MODE);

# This is to test the phrase search, searching phrase
# "following database"
SELECT * FROM articles
        WHERE MATCH (title,body)
        AGAINST ('"following database"' IN BOOLEAN MODE);
}

if ($basic_stage == insert_3)
{
# Insert into table with similar word of different distances
INSERT INTO articles (title,body) VALUES
	('test proximity search, test, proximity and phrase',
	 'search, with proximity innodb');

INSERT INTO articles (title,body) VALUES
	('test my proximity fts new search, test, proximity and phrase',
	 'search, with proximity innodb');

INSERT INTO articles (title,body) VALUES
	('test more of proximity fts search, test, more proximity and phrase',
	 'search, with proximity innodb');
}

if ($basic_stage == select_3)
{
# This should only return the first document
SELECT * FROM articles
	WHERE MATCH (title,body)
	AGAINST ('"proximity search"@3' IN BOOLEAN MODE);

# This would return no document
SELECT * FROM articles
	WHERE MATCH (title,body)
	AGAINST ('"proximity search"@2' IN BOOLEAN MODE);

# This give you all three documents
SELECT * FROM articles
	WHERE MATCH (title,body)
	AGAINST ('"proximity search"@5' IN BOOLEAN MODE);

# Similar boundary testing for the words
SELECT * FROM articles
        WHERE MATCH (title,body)
	AGAINST ('"test proximity"@5' IN BOOLEAN MODE);

# No document will be returned
SELECT * FROM articles
        WHERE MATCH (title,body)
	AGAINST ('"test proximity"@1' IN BOOLEAN MODE);

# All three documents will be returned
SELECT * FROM articles
        WHERE MATCH (title,body)
	AGAINST ('"test proximity"@4' IN BOOLEAN MODE);

# Only two document will be returned.
SELECT * FROM articles
        WHERE MATCH (title,body)
	AGAINST ('"test proximity"@3' IN BOOLEAN MODE);

# Test with more word The last document will return, please notice there
# is no ordering requirement for proximity search.
SELECT * FROM articles
        WHERE MATCH (title,body)
	AGAINST ('"more test proximity"@4' IN BOOLEAN MODE);

SELECT * FROM articles
        WHERE MATCH (title,body)
	AGAINST ('"more test proximity"@3' IN BOOLEAN MODE);

# The phrase search will not require exact word ordering
SELECT * FROM articles
        WHERE MATCH (title,body)
	AGAINST ('"more test proximity"' IN BOOLEAN MODE);
}
