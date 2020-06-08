INSERT INTO users VALUES ('uAlfonso', 'Alfonso', '9299032533');
INSERT INTO users VALUES ('uDarius', 'Darius', '9448448036');
INSERT INTO users VALUES ('uGil', 'Gil', '9456779000');
INSERT INTO users VALUES ('uHilel', 'Hilel', '9648349557');
INSERT INTO users VALUES ('uIvy', 'Ivy', '9548480865');
INSERT INTO users VALUES ('uLilah', 'Lilah', '9011806473');
INSERT INTO users VALUES ('uMoses', 'Moses', '9113712652');
INSERT INTO users VALUES ('uNicole', 'Nicole', '9108214189');
INSERT INTO users VALUES ('uSavannah', 'Savannah', '9269411303');
INSERT INTO users VALUES ('uSheila', 'Sheila', '9102157272');
INSERT INTO users VALUES ('uSimon', 'Simon', '9408542546');
INSERT INTO users VALUES ('uSteel', 'Steel', '9107543266');
INSERT INTO users VALUES ('uYeo', 'Yeo', '9832571595');

INSERT INTO password VALUES ('uAlfonso', 'pAlfonso');
INSERT INTO password VALUES ('uDarius', 'pDarius');
INSERT INTO password VALUES ('uGil', 'pGil');
INSERT INTO password VALUES ('uHilel', 'pHilel');
INSERT INTO password VALUES ('uIvy', 'pIvy');
INSERT INTO password VALUES ('uLilah', 'pLilah');
INSERT INTO password VALUES ('uMoses', 'pMoses');
INSERT INTO password VALUES ('uNicole', 'pNicole');
INSERT INTO password VALUES ('uSavannah', 'pSavannah');
INSERT INTO password VALUES ('uSheila', 'pSheila');
INSERT INTO password VALUES ('uSimon', 'pSimon');
INSERT INTO password VALUES ('uSteel', 'pSteel');
INSERT INTO password VALUES ('uYeo', 'pYeo');

/* For Bonus */
create table lastseen (
     uid varchar(10) references users,
     thread_id integer references conversations(thread_id),
     primary key (uid, thread_id),
	 lastseen_post_id integer references posts(post_id)
);
