-- Uncomment if needed
-- create database whatasap;

create table users(
    uid varchar(10) primary key,
    name varchar(20),
    phone varchar(10));

create table conversations(
     uid1 varchar(10) references users,
     uid2 varchar(10) references users,
     thread_id serial,
     primary key (uid1, uid2),
     unique(thread_id),
     check (uid1 < uid2));

create table posts (
    post_id serial primary key,
    thread_id integer references conversations(thread_id),
    uid varchar(10) references users,
    timestamp timestamp,
    text varchar(256)
);

create table password (
  uid varchar(10) references users,
  password varchar(20)
);

INSERT INTO users (uid, name, phone) VALUES ('p1', 'Person1', '9999999999');
INSERT INTO users (uid, name, phone) VALUES ('p2', 'Person2', '8999999999');
INSERT INTO users (uid, name, phone) VALUES ('p3', 'Person1', '7999999999');
INSERT INTO users (uid, name, phone) VALUES ('p4', 'Person4', '6999999999');
INSERT INTO users (uid, name, phone) VALUES ('p5', 'Person5', '5999999999');
INSERT INTO users (uid, name, phone) VALUES ('p6', 'Person6', '4999999999');
INSERT INTO users (uid, name, phone) VALUES ('p7', 'Person7', '3999999999');
INSERT INTO users (uid, name, phone) VALUES ('p8', 'Person8', '2999999999');

insert into password values ('p1', 'Person1');
insert into password values ('p2', 'Person2');
insert into password values ('p3', 'Person3');
insert into password values ('p4', 'Person4');
insert into password values ('p5', 'Person5');
insert into password values ('p6', 'Person6');
insert into password values ('p7', 'Person7');
insert into password values ('p8', 'Person8');

insert into conversations (uid1, uid2) VALUES ('p1', 'p2');
insert into conversations (uid1, uid2) VALUES ('p1', 'p3');
insert into conversations (uid1, uid2) VALUES ('p2', 'p4');

insert into posts (thread_id, uid, timestamp, text) values (1, 'p1', now(), 'Hey p2 how are you?.');
insert into posts (thread_id, uid, timestamp, text) values (1, 'p2', now(), 'Hey p1 I am good.');
insert into posts (thread_id, uid, timestamp, text) values (1, 'p1', now(), 'Lets meet up this weekend.');
insert into posts (thread_id, uid, timestamp, text) values (1, 'p2', now(), 'Sure lets do that.');
insert into posts (thread_id, uid, timestamp, text) values (3, 'p4', now(), 'This is p4 messaging p2.');
