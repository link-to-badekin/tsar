int foo(){
	return 91;
}



int main(){
	int x = 0, y;

#pragma spf transform inline
	{
		x = foo() + 2 * foo();
	}


	return 0;
}
//CHECK: 
